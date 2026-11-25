#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>

namespace {

std::wstring GetKnownDir(REFKNOWNFOLDERID rfid) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, nullptr, &p)) && p) {
        std::wstring s(p);
        CoTaskMemFree(p);
        return s;
    }
    return L"";
}

std::wstring GetSelfDir() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4 - 1);
    std::wstring full(buf, n);
    size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : full.substr(0, pos);
}

std::wstring GetSelfPath() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4 - 1);
    return std::wstring(buf, n);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR lpCmdLine, int) {
    std::wstring cmd = lpCmdLine ? lpCmdLine : L"";
    bool silent = (cmd.find(L"/S") != std::wstring::npos || cmd.find(L"/s") != std::wstring::npos);

    if (!silent) {
        int ans = MessageBoxW(
            nullptr,
            L"Are you sure you want to completely remove VPN Echo 1.1.1.1 and all of its components?",
            L"VPN Echo Uninstall",
            MB_YESNO | MB_ICONQUESTION
        );
        if (ans != IDYES) return 0;
    }

    std::wstring instDir = GetSelfDir();
    const wchar_t* files[] = {
        L"VPN Echo.exe",
        L"sing-box.exe",
        L"libcronet.dll",
        L"servers.json",
        L"echo.ico",
        L"sing-box-LICENSE.txt",
        L"GPL-3.0.txt"
    };
    for (const wchar_t* f : files) {
        std::wstring p = instDir + L"\\" + f;
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
    }

    std::wstring commonProgs = GetKnownDir(FOLDERID_CommonPrograms);
    if (!commonProgs.empty()) {
        std::wstring smDir = commonProgs + L"\\VPN Echo";
        DeleteFileW((smDir + L"\\VPN Echo.lnk").c_str());
        DeleteFileW((smDir + L"\\Uninstall VPN Echo.lnk").c_str());
        RemoveDirectoryW(smDir.c_str());
    }

    std::wstring pubDesktop = GetKnownDir(FOLDERID_PublicDesktop);
    if (!pubDesktop.empty()) {
        DeleteFileW((pubDesktop + L"\\VPN Echo.lnk").c_str());
    }

    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VPN Echo", KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\VPN Echo", KEY_WOW64_64KEY, 0);

    // Self-remove Uninstall.exe and install directory via short delayed cmd
    std::wstring selfExe = GetSelfPath();
    std::wstring cleanupCmd = L"cmd.exe /c ping 127.0.0.1 -n 2 > nul & del /f /q \"" + selfExe + L"\" & rmdir \"" + instDir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cleanupCmd;
    mutableCmd.push_back(L'\0');
    if (CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, L"C:\\", &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    if (!silent) {
        MessageBoxW(nullptr, L"VPN Echo was successfully removed from your computer.", L"VPN Echo Uninstall", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}
