#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <objbase.h>
#include <objidl.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <windowsx.h>

#include "inflate.hpp"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define IDI_APPICON 101

namespace {

using namespace Gdiplus;

constexpr UINT WM_SETUP_PROGRESS = WM_APP + 10;
constexpr UINT WM_SETUP_DONE     = WM_APP + 11;
constexpr int  IDC_PATH_EDIT     = 3001;

#pragma pack(push, 1)
struct PayloadFooter {
    uint64_t offset;
    uint64_t size;
    uint8_t  sha256[32];
    char     magic[8]; // "VPNECHO1"
};
#pragma pack(pop)

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

std::wstring GetKnownDir(REFKNOWNFOLDERID rfid) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, nullptr, &p)) && p) {
        std::wstring s(p);
        CoTaskMemFree(p);
        return s;
    }
    return L"";
}

std::wstring DefaultInstallDir() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\VPN Echo", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH * 2]{};
        DWORD sz = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"InstallDir", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0] != L'\0') {
            RegCloseKey(hKey);
            return buf;
        }
        RegCloseKey(hKey);
    }
    std::wstring pf = GetKnownDir(FOLDERID_ProgramFilesX64);
    if (pf.empty()) {
        wchar_t env[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(L"ProgramW6432", env, MAX_PATH * 2) > 0) pf = env;
        else if (GetEnvironmentVariableW(L"ProgramFiles", env, MAX_PATH * 2) > 0) pf = env;
        else pf = L"C:\\Program Files";
    }
    return pf + L"\\VPN Echo";
}

bool ComputeSha256(const void* data, size_t len, uint8_t out32[32]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    NTSTATUS st = BCryptHash(hAlg, nullptr, 0,
                             const_cast<PUCHAR>(static_cast<const uint8_t*>(data)),
                             static_cast<ULONG>(len), out32, 32);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return st == 0;
}

bool WriteAllBytes(const std::wstring& path, const uint8_t* data, size_t len) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t pos = 0;
    while (pos < len) {
        DWORD chunk = 0;
        DWORD ask = static_cast<DWORD>(std::min<size_t>(len - pos, 1 << 20));
        if (!WriteFile(h, data + pos, ask, &chunk, nullptr) || chunk == 0) {
            CloseHandle(h);
            return false;
        }
        pos += chunk;
    }
    CloseHandle(h);
    return true;
}

bool CreateShortcutLnk(const std::wstring& lnkPath, const std::wstring& targetPath,
                       const std::wstring& workDir, const std::wstring& iconPath) {
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&psl));
    if (FAILED(hr) || !psl) return false;
    psl->SetPath(targetPath.c_str());
    psl->SetWorkingDirectory(workDir.c_str());
    if (!iconPath.empty()) psl->SetIconLocation(iconPath.c_str(), 0);

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
    bool ok = false;
    if (SUCCEEDED(hr) && ppf) {
        ok = SUCCEEDED(ppf->Save(lnkPath.c_str(), TRUE));
        ppf->Release();
    }
    psl->Release();
    return ok;
}

void WriteUninstallRegistry(const std::wstring& instDir) {
    HKEY hAppKey = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\VPN Echo", 0, nullptr, 0,
                        KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hAppKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hAppKey, L"InstallDir", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(instDir.c_str()),
                       static_cast<DWORD>((instDir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hAppKey);
    }

    HKEY hUninst = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VPN Echo",
                        0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hUninst, nullptr) == ERROR_SUCCESS) {
        auto setStr = [&](const wchar_t* name, const std::wstring& val) {
            RegSetValueExW(hUninst, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(val.c_str()),
                           static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        };
        setStr(L"DisplayName", L"VPN Echo");
        setStr(L"UninstallString", L"\"" + instDir + L"\\Uninstall.exe\"");
        setStr(L"DisplayVersion", L"1.1.1.1");
        setStr(L"Publisher", L"Ramtin Ahmadi");
        setStr(L"DisplayIcon", instDir + L"\\echo.ico");
        DWORD one = 1;
        RegSetValueExW(hUninst, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        RegSetValueExW(hUninst, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        RegCloseKey(hUninst);
    }
}

bool PerformInstallation(const std::wstring& instDir,
                         std::function<void(int pct, const std::wstring& status)> onProgress,
                         std::wstring& outErr) {
    wchar_t selfPath[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH * 4 - 1);
    HANDLE hSelf = CreateFileW(selfPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hSelf == INVALID_HANDLE_VALUE) {
        outErr = L"Could not open installer executable.";
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hSelf, &fileSize) || fileSize.QuadPart < static_cast<LONGLONG>(sizeof(PayloadFooter))) {
        CloseHandle(hSelf);
        outErr = L"Installer package is incomplete.";
        return false;
    }

    LARGE_INTEGER footerPos{};
    footerPos.QuadPart = fileSize.QuadPart - static_cast<LONGLONG>(sizeof(PayloadFooter));
    SetFilePointerEx(hSelf, footerPos, nullptr, FILE_BEGIN);
    PayloadFooter footer{};
    DWORD rd = 0;
    if (!ReadFile(hSelf, &footer, sizeof(footer), &rd, nullptr) || rd != sizeof(footer) ||
        std::memcmp(footer.magic, "VPNECHO1", 8) != 0) {
        CloseHandle(hSelf);
        outErr = L"Installer payload signature is invalid.";
        return false;
    }

    if (footer.offset + footer.size + sizeof(PayloadFooter) > static_cast<uint64_t>(fileSize.QuadPart) ||
        footer.size == 0 || footer.size > (128ULL * 1024 * 1024)) {
        CloseHandle(hSelf);
        outErr = L"Installer payload bounds are invalid.";
        return false;
    }

    if (onProgress) onProgress(5, L"Verifying installer integrity\u2026");

    std::vector<uint8_t> archive(static_cast<size_t>(footer.size));
    LARGE_INTEGER payPos{};
    payPos.QuadPart = static_cast<LONGLONG>(footer.offset);
    SetFilePointerEx(hSelf, payPos, nullptr, FILE_BEGIN);
    size_t got = 0;
    while (got < archive.size()) {
        DWORD chunk = 0;
        DWORD ask = static_cast<DWORD>(std::min<size_t>(archive.size() - got, 1 << 20));
        if (!ReadFile(hSelf, archive.data() + got, ask, &chunk, nullptr) || chunk == 0) {
            CloseHandle(hSelf);
            outErr = L"Failed reading installer payload.";
            return false;
        }
        got += chunk;
    }
    CloseHandle(hSelf);

    uint8_t digest[32]{};
    if (!ComputeSha256(archive.data(), archive.size(), digest) ||
        std::memcmp(digest, footer.sha256, 32) != 0) {
        outErr = L"Installer SHA-256 verification failed.";
        return false;
    }

    SHCreateDirectoryExW(nullptr, instDir.c_str(), nullptr);
    DWORD attr = GetFileAttributesW(instDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        outErr = L"Could not create target folder:\n" + instDir;
        return false;
    }

    size_t cursor = 0;
    auto readBytes = [&](void* dst, size_t n) -> bool {
        if (cursor + n > archive.size()) return false;
        std::memcpy(dst, archive.data() + cursor, n);
        cursor += n;
        return true;
    };

    uint32_t fileCount = 0;
    if (!readBytes(&fileCount, sizeof(fileCount)) || fileCount == 0 || fileCount > 32) {
        outErr = L"Corrupt archive header.";
        return false;
    }

    std::vector<uint8_t> decoded;
    for (uint32_t i = 0; i < fileCount; ++i) {
        uint32_t nameLen = 0;
        if (!readBytes(&nameLen, sizeof(nameLen)) || nameLen == 0 || nameLen > 260) {
            outErr = L"Corrupt entry name in archive.";
            return false;
        }
        std::string nameUtf8(nameLen, '\0');
        if (!readBytes(&nameUtf8[0], nameLen)) {
            outErr = L"Unexpected end of archive.";
            return false;
        }
        uint64_t rawSize = 0, compSize = 0;
        if (!readBytes(&rawSize, sizeof(rawSize)) || !readBytes(&compSize, sizeof(compSize)) ||
            cursor + compSize > archive.size()) {
            outErr = L"Corrupt entry size in archive.";
            return false;
        }

        std::wstring wname = ToWide(nameUtf8);
        int pct = 10 + static_cast<int>(((i + 1) * 78) / fileCount);
        if (onProgress) onProgress(pct, L"Installing " + wname + L"\u2026");

        try {
            vpnecho::ZlibInflater inf(archive.data() + cursor, static_cast<size_t>(compSize));
            inf.DecompressZlib(decoded, static_cast<size_t>(rawSize));
        } catch (const std::exception& ex) {
            outErr = L"Decompression failed for " + wname + L": " + ToWide(ex.what());
            return false;
        }
        cursor += static_cast<size_t>(compSize);

        if (decoded.size() != rawSize) {
            outErr = L"Unpacked size mismatch for " + wname;
            return false;
        }
        std::wstring outPath = instDir + L"\\" + wname;
        if (!WriteAllBytes(outPath, decoded.data(), decoded.size())) {
            outErr = L"Could not write file (check if VPN Echo is currently running):\n" + outPath;
            return false;
        }
    }

    if (onProgress) onProgress(93, L"Creating shortcuts and registering application\u2026");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring appExe = instDir + L"\\VPN Echo.exe";
    std::wstring uninstExe = instDir + L"\\Uninstall.exe";
    std::wstring icoPath = instDir + L"\\echo.ico";

    std::wstring commonProgs = GetKnownDir(FOLDERID_CommonPrograms);
    if (!commonProgs.empty()) {
        std::wstring smDir = commonProgs + L"\\VPN Echo";
        SHCreateDirectoryExW(nullptr, smDir.c_str(), nullptr);
        CreateShortcutLnk(smDir + L"\\VPN Echo.lnk", appExe, instDir, icoPath);
        CreateShortcutLnk(smDir + L"\\Uninstall VPN Echo.lnk", uninstExe, instDir, icoPath);
    }
    std::wstring pubDesktop = GetKnownDir(FOLDERID_PublicDesktop);
    if (!pubDesktop.empty()) {
        CreateShortcutLnk(pubDesktop + L"\\VPN Echo.lnk", appExe, instDir, icoPath);
    }
    CoUninitialize();

    WriteUninstallRegistry(instDir);
    if (onProgress) onProgress(100, L"VPN Echo 1.1.1.1 has been installed successfully.");
    return true;
}

void AddRoundRect(GraphicsPath& path, const RectF& r, REAL rad) {
    REAL d = std::min({rad * 2.0f, r.Width, r.Height});
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

class SetupWindow {
    enum class Page { Welcome, Installing, Finished };
    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND hPathEdit_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HFONT editFont_ = nullptr;
    float scale_ = 1.0f;

    Page page_ = Page::Welcome;
    std::wstring installDir_;
    int progressPct_ = 0;
    std::wstring statusText_ = L"Ready to install VPN Echo 1.1.1.1 (64-bit).";
    bool launchOnFinish_ = true;
    int hoverBtn_ = 0; // 1=Primary, 2=Close/Cancel, 3=Browse, 4=Checkbox

    std::mutex stateMtx_;

    float S(float v) const { return v * scale_; }
    int   SI(float v) const { return static_cast<int>(std::lround(v * scale_)); }

    RectF CloseBtnRect()   const { return RectF(S(438.0f), S(12.0f), S(30.0f), S(30.0f)); }
    RectF BrowseBtnRect()  const { return RectF(S(382.0f), S(162.0f), S(74.0f), S(38.0f)); }
    RectF PrimaryBtnRect() const { return RectF(S(24.0f),  S(248.0f), S(432.0f), S(46.0f)); }
    RectF CheckRect()      const { return RectF(S(24.0f),  S(198.0f), S(240.0f), S(28.0f)); }

    void BrowseFolder() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IFileDialog* pfd = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))) && pfd) {
            DWORD opts = 0;
            pfd->GetOptions(&opts);
            pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            pfd->SetTitle(L"Select VPN Echo Installation Folder");
            if (SUCCEEDED(pfd->Show(hwnd_))) {
                IShellItem* psi = nullptr;
                if (SUCCEEDED(pfd->GetResult(&psi)) && psi) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        std::wstring chosen(path);
                        CoTaskMemFree(path);
                        if (chosen.find(L"VPN Echo") == std::wstring::npos) {
                            if (!chosen.empty() && chosen.back() != L'\\') chosen += L"\\";
                            chosen += L"VPN Echo";
                        }
                        SetWindowTextW(hPathEdit_, chosen.c_str());
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
        CoUninitialize();
    }

    void StartInstall() {
        wchar_t buf[MAX_PATH * 2]{};
        GetWindowTextW(hPathEdit_, buf, MAX_PATH * 2 - 1);
        installDir_ = buf;
        if (installDir_.empty()) installDir_ = DefaultInstallDir();

        page_ = Page::Installing;
        ShowWindow(hPathEdit_, SW_HIDE);
        InvalidateRect(hwnd_, nullptr, FALSE);

        std::wstring target = installDir_;
        std::thread([this, target]() {
            std::wstring err;
            bool ok = PerformInstallation(
                target,
                [this](int pct, const std::wstring& st) {
                    {
                        std::lock_guard<std::mutex> lk(stateMtx_);
                        progressPct_ = pct;
                        statusText_ = st;
                    }
                    PostMessageW(hwnd_, WM_SETUP_PROGRESS, 0, 0);
                },
                err
            );
            if (!ok) {
                std::lock_guard<std::mutex> lk(stateMtx_);
                statusText_ = err;
            }
            PostMessageW(hwnd_, WM_SETUP_DONE, ok ? 1 : 0, 0);
        }).detach();
    }

    void Paint(HDC hdc) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        Bitmap bmp(w, h, PixelFormat32bppARGB);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        g.Clear(Color(255, 0x11, 0x13, 0x15));

        SolidBrush brInk(Color(255, 0xD2, 0xD6, 0xD8));
        SolidBrush brMuted(Color(255, 0x89, 0x91, 0x96));
        SolidBrush brCard(Color(255, 0x1D, 0x21, 0x24));
        SolidBrush brGreen(Color(255, 0x66, 0xC9, 0x95));
        SolidBrush brDarkGreen(Color(255, 32, 88, 62));

        FontFamily ff(L"Segoe UI");
        Font fBrand(&ff, S(24.0f), FontStyleBold, UnitPixel);
        Font fSub(&ff, S(12.5f), FontStyleRegular, UnitPixel);
        Font fLabel(&ff, S(11.5f), FontStyleRegular, UnitPixel);
        Font fBtn(&ff, S(14.5f), FontStyleBold, UnitPixel);

        StringFormat sfLeft;
        sfLeft.SetAlignment(StringAlignmentNear);
        sfLeft.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfCenter;
        sfCenter.SetAlignment(StringAlignmentCenter);
        sfCenter.SetLineAlignment(StringAlignmentCenter);

        // Header
        g.DrawString(L"echo", -1, &fBrand, RectF(S(24.0f), S(12.0f), S(120.0f), S(36.0f)), &sfLeft, &brInk);
        g.DrawString(L"VPN Echo 1.1.1.1 Setup", -1, &fLabel, RectF(S(102.0f), S(16.0f), S(220.0f), S(32.0f)), &sfLeft, &brMuted);

        if (page_ != Page::Installing) {
            RectF rClose = CloseBtnRect();
            if (hoverBtn_ == 2) {
                GraphicsPath p; AddRoundRect(p, rClose, S(6.0f));
                SolidBrush bh(Color(255, 0x25, 0x2A, 0x2D));
                g.FillPath(&bh, &p);
            }
            g.DrawString(L"\u00D7", -1, &fBtn, rClose, &sfCenter, hoverBtn_ == 2 ? &brInk : &brMuted);
        }

        int pct = 0;
        std::wstring stText;
        {
            std::lock_guard<std::mutex> lk(stateMtx_);
            pct = progressPct_;
            stText = statusText_;
        }

        if (page_ == Page::Welcome) {
            g.DrawString(L"Install VPN Echo 1.1.1.1 (Windows 10/11 x64) including the bundled\nsing-box 1.14.4 VPN engine, Desktop shortcut, and Start Menu entry.",
                         -1, &fSub, RectF(S(24.0f), S(68.0f), S(432.0f), S(48.0f)), &sfLeft, &brMuted);

            g.DrawString(L"Destination folder:", -1, &fLabel, RectF(S(24.0f), S(136.0f), S(300.0f), S(22.0f)), &sfLeft, &brInk);

            GraphicsPath pBox;
            AddRoundRect(pBox, RectF(S(24.0f), S(162.0f), S(350.0f), S(38.0f)), S(8.0f));
            g.FillPath(&brCard, &pBox);

            RectF rBrw = BrowseBtnRect();
            GraphicsPath pBrw;
            AddRoundRect(pBrw, rBrw, S(8.0f));
            SolidBrush brBrw(hoverBtn_ == 3 ? Color(255, 0x36, 0x40, 0x43) : Color(255, 0x2C, 0x34, 0x36));
            g.FillPath(&brBrw, &pBrw);
            g.DrawString(L"Browse", -1, &fLabel, rBrw, &sfCenter, &brInk);

            RectF rPri = PrimaryBtnRect();
            GraphicsPath pPri;
            AddRoundRect(pPri, rPri, S(10.0f));
            SolidBrush brPri(hoverBtn_ == 1 ? Color(255, 38, 104, 74) : Color(255, 32, 88, 62));
            g.FillPath(&brPri, &pPri);
            Pen penPri(Color(255, 0x66, 0xC9, 0x95), 1.0f);
            g.DrawPath(&penPri, &pPri);
            g.DrawString(L"Install VPN Echo", -1, &fBtn, rPri, &sfCenter, &brInk);
        } else if (page_ == Page::Installing) {
            g.DrawString(stText.c_str(), -1, &fSub, RectF(S(24.0f), S(118.0f), S(432.0f), S(32.0f)), &sfLeft, &brInk);

            RectF rTrack(S(24.0f), S(164.0f), S(432.0f), S(12.0f));
            GraphicsPath pTrack;
            AddRoundRect(pTrack, rTrack, S(6.0f));
            g.FillPath(&brCard, &pTrack);

            float fw = rTrack.Width * (static_cast<float>(std::max(3, pct)) / 100.0f);
            GraphicsPath pFill;
            AddRoundRect(pFill, RectF(rTrack.X, rTrack.Y, fw, rTrack.Height), S(6.0f));
            g.FillPath(&brGreen, &pFill);

            std::wstring pctStr = std::to_wstring(pct) + L"%";
            g.DrawString(pctStr.c_str(), -1, &fLabel, RectF(S(24.0f), S(186.0f), S(432.0f), S(24.0f)), &sfLeft, &brMuted);
        } else {
            g.DrawString(L"Installation Complete", -1, &fBtn, RectF(S(24.0f), S(82.0f), S(432.0f), S(28.0f)), &sfLeft, &brGreen);
            g.DrawString(L"VPN Echo 1.1.1.1 has been installed on your computer.\nShortcuts have been added to your Desktop and Start Menu.",
                         -1, &fSub, RectF(S(24.0f), S(116.0f), S(432.0f), S(48.0f)), &sfLeft, &brMuted);

            RectF rChkBox(S(24.0f), S(202.0f), S(20.0f), S(20.0f));
            GraphicsPath pChk;
            AddRoundRect(pChk, rChkBox, S(5.0f));
            g.FillPath(launchOnFinish_ ? &brDarkGreen : &brCard, &pChk);
            Pen penChk(launchOnFinish_ ? Color(255, 0x66, 0xC9, 0x95) : Color(255, 0x45, 0x4E, 0x52), 1.2f);
            g.DrawPath(&penChk, &pChk);
            if (launchOnFinish_) {
                g.DrawString(L"\u2713", -1, &fLabel, rChkBox, &sfCenter, &brGreen);
            }
            g.DrawString(L"Launch VPN Echo now", -1, &fSub, RectF(S(52.0f), S(198.0f), S(220.0f), S(28.0f)), &sfLeft, &brInk);

            RectF rPri = PrimaryBtnRect();
            GraphicsPath pPri;
            AddRoundRect(pPri, rPri, S(10.0f));
            SolidBrush brPri(hoverBtn_ == 1 ? Color(255, 38, 104, 74) : Color(255, 32, 88, 62));
            g.FillPath(&brPri, &pPri);
            Pen penPri(Color(255, 0x66, 0xC9, 0x95), 1.0f);
            g.DrawPath(&penPri, &pPri);
            g.DrawString(L"Finish", -1, &fBtn, rPri, &sfCenter, &brInk);
        }

        Pen borderPen(Color(255, 0x30, 0x36, 0x39), 1.0f);
        g.DrawRectangle(&borderPen, 0.5f, 0.5f, static_cast<REAL>(w) - 1.0f, static_cast<REAL>(h) - 1.0f);

        Graphics sg(hdc);
        sg.DrawImage(&bmp, 0, 0);
    }

    int HitBtn(float x, float y) const {
        if (page_ != Page::Installing && CloseBtnRect().Contains(x, y)) return 2;
        if (page_ == Page::Welcome) {
            if (PrimaryBtnRect().Contains(x, y)) return 1;
            if (BrowseBtnRect().Contains(x, y)) return 3;
        } else if (page_ == Page::Finished) {
            if (PrimaryBtnRect().Contains(x, y)) return 1;
            if (CheckRect().Contains(x, y)) return 4;
        }
        return 0;
    }

    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE: {
                HDC dc = GetDC(hwnd_);
                scale_ = std::max(1.0f, static_cast<float>(GetDeviceCaps(dc, LOGPIXELSX)) / 96.0f);
                ReleaseDC(hwnd_, dc);

                installDir_ = DefaultInstallDir();
                editBrush_ = CreateSolidBrush(RGB(0x1D, 0x21, 0x24));
                editFont_ = CreateFontW(-SI(13.0f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                hPathEdit_ = CreateWindowExW(
                    0, L"EDIT", installDir_.c_str(),
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                    SI(34.0f), SI(171.0f), SI(330.0f), SI(20.0f),
                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PATH_EDIT)), hInst_, nullptr
                );
                SendMessageW(hPathEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
                return 0;
            }
            case WM_CTLCOLOREDIT: {
                HDC hdcEdit = reinterpret_cast<HDC>(wParam);
                SetTextColor(hdcEdit, RGB(0xD2, 0xD6, 0xD8));
                SetBkColor(hdcEdit, RGB(0x1D, 0x21, 0x24));
                return reinterpret_cast<LRESULT>(editBrush_);
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd_, &ps);
                Paint(hdc);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_SETCURSOR: {
                if (LOWORD(lParam) == HTCLIENT && hoverBtn_ != 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                break;
            }
            case WM_MOUSEMOVE: {
                int nb = HitBtn(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
                if (nb != hoverBtn_) {
                    hoverBtn_ = nb;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONDOWN: {
                float mx = static_cast<float>(GET_X_LPARAM(lParam));
                float my = static_cast<float>(GET_Y_LPARAM(lParam));
                int b = HitBtn(mx, my);
                if (b == 0 && my < S(52.0f)) {
                    ReleaseCapture();
                    SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    return 0;
                }
                if (b == 2 && page_ != Page::Installing) {
                    DestroyWindow(hwnd_);
                } else if (b == 3 && page_ == Page::Welcome) {
                    BrowseFolder();
                } else if (b == 1 && page_ == Page::Welcome) {
                    StartInstall();
                } else if (b == 4 && page_ == Page::Finished) {
                    launchOnFinish_ = !launchOnFinish_;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else if (b == 1 && page_ == Page::Finished) {
                    if (launchOnFinish_) {
                        std::wstring exe = installDir_ + L"\\VPN Echo.exe";
                        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, installDir_.c_str(), SW_SHOWNORMAL);
                    }
                    DestroyWindow(hwnd_);
                }
                return 0;
            }
            case WM_SETUP_PROGRESS:
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_SETUP_DONE: {
                if (wParam == 1) {
                    page_ = Page::Finished;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else {
                    std::wstring err;
                    {
                        std::lock_guard<std::mutex> lk(stateMtx_);
                        err = statusText_;
                    }
                    MessageBoxW(hwnd_, err.c_str(), L"VPN Echo Setup", MB_OK | MB_ICONERROR);
                    page_ = Page::Welcome;
                    ShowWindow(hPathEdit_, SW_SHOW);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            case WM_DESTROY: {
                if (editFont_) DeleteObject(editFont_);
                if (editBrush_) DeleteObject(editBrush_);
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        SetupWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<SetupWindow*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->WndProc(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

public:
    int RunGui(HINSTANCE hInst, int nCmdShow) {
        hInst_ = hInst;
        HICON hIcon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &StaticProc;
        wc.hInstance = hInst;
        wc.hIcon = hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"VpnEchoSetupClass";
        RegisterClassExW(&wc);

        HDC sdc = GetDC(nullptr);
        float sc = std::max(1.0f, static_cast<float>(GetDeviceCaps(sdc, LOGPIXELSX)) / 96.0f);
        int ww = static_cast<int>(std::lround(480.0f * sc));
        int wh = static_cast<int>(std::lround(320.0f * sc));
        int px = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2);
        int py = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2);
        ReleaseDC(nullptr, sdc);

        hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"VPN Echo 1.1.1.1 Setup",
                                WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                                px, py, ww, wh, nullptr, nullptr, hInst, this);
        if (!hwnd_) return 1;
        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);

        MSG m{};
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        return 0;
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    std::wstring cmd = lpCmdLine ? lpCmdLine : L"";
    bool silent = false;
    std::wstring customDir;

    // Support NSIS-compatible /S and /D=<dir> flags
    size_t dPos = cmd.find(L"/D=");
    if (dPos != std::wstring::npos) {
        customDir = cmd.substr(dPos + 3);
        while (!customDir.empty() && (customDir.front() == L' ' || customDir.front() == L'"')) customDir.erase(customDir.begin());
        while (!customDir.empty() && (customDir.back() == L' ' || customDir.back() == L'"')) customDir.pop_back();
    }
    if (cmd.find(L"/S") != std::wstring::npos) silent = true;

    if (silent) {
        std::wstring dir = customDir.empty() ? DefaultInstallDir() : customDir;
        std::wstring err;
        return PerformInstallation(dir, nullptr, err) ? 0 : 1;
    }

    GdiplusStartupInput gsi;
    ULONG_PTR tok = 0;
    GdiplusStartup(&tok, &gsi, nullptr);
    SetupWindow win;
    int rc = win.RunGui(hInst, nCmdShow);
    GdiplusShutdown(tok);
    return rc;
}
