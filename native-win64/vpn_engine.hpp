#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <sspi.h>
#include <schannel.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "common.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#define SP_PROT_TLS1_3_CLIENT 0x00002000
#endif
#ifndef SCH_USE_STRONG_CRYPTO
#define SCH_USE_STRONG_CRYPTO 0x00400000
#endif

namespace vpnecho {

inline std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

inline std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
    return out;
}

inline bool FileExistsW(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

inline void EnsureDirTreeW(const std::wstring& path) {
    SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
}

inline bool ReadAllBytesW(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (128LL * 1024 * 1024)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD total = 0;
    while (total < out.size()) {
        DWORD chunk = 0;
        if (!ReadFile(h, out.data() + total, static_cast<DWORD>(out.size() - total), &chunk, nullptr) || chunk == 0) break;
        total += chunk;
    }
    CloseHandle(h);
    return total == out.size();
}

inline bool WriteAllBytesW(const std::wstring& path, const void* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t writtenTotal = 0;
    while (writtenTotal < len) {
        DWORD chunk = 0;
        DWORD toWrite = static_cast<DWORD>(std::min<size_t>(len - writtenTotal, 1 << 20));
        if (!WriteFile(h, ptr + writtenTotal, toWrite, &chunk, nullptr) || chunk == 0) {
            CloseHandle(h);
            return false;
        }
        writtenTotal += chunk;
    }
    CloseHandle(h);
    return true;
}

struct DataPaths {
    static std::wstring BaseDir() {
        wchar_t buf[MAX_PATH * 4]{};
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4 - 1);
        std::wstring full(buf, n);
        size_t slash = full.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? L"." : full.substr(0, slash);
    }

    static std::wstring Root() {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) {
            std::wstring res = std::wstring(p) + L"\\VPN Echo";
            CoTaskMemFree(p);
            return res;
        }
        wchar_t env[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH * 2) > 0) {
            return std::wstring(env) + L"\\VPN Echo";
        }
        return BaseDir() + L"\\data";
    }

    static std::wstring Configs() { return Root() + L"\\configs"; }
    static std::wstring Logs()    { return Root() + L"\\logs"; }
    static std::wstring CoreExe() { return BaseDir() + L"\\sing-box.exe"; }
    static std::wstring ServersJson() { return BaseDir() + L"\\servers.json"; }
    static std::wstring IconPath() { return BaseDir() + L"\\echo.ico"; }

    static void Ensure() {
        EnsureDirTreeW(Root());
        EnsureDirTreeW(Configs());
        EnsureDirTreeW(Logs());
    }

    static std::wstring StampUtc() {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        static std::atomic<uint32_t> seq{0};
        uint32_t s = (++seq) % 1000;
        wchar_t buf[64];
        std::swprintf(buf, 64, L"%04u%02u%02u-%02u%02u%02u-%03u-%03u",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, s);
        return buf;
    }

    static std::wstring UniqueConfig(const std::wstring& mode, int id) {
        return Configs() + L"\\" + mode + L"-" + std::to_wstring(id + 1) + L"-" + StampUtc() + L".json";
    }

    static std::wstring UniqueLog(const std::wstring& mode, int id) {
        return Logs() + L"\\" + mode + L"-" + std::to_wstring(id + 1) + L"-" + StampUtc() + L".log";
    }
};

struct LicenseStore {
    // Exact SHA-256 from LicenseStore.cs: 8848baf57c30e8a91a9ecd6b893a64a4564959eed47049fce10d0ebbe871ef3d
    static const uint8_t* ExpectedBytes() {
        static const uint8_t kExpected[32] = {
            0x88,0x48,0xba,0xf5,0x7c,0x30,0xe8,0xa9,
            0x1a,0x9e,0xcd,0x6b,0x89,0x3a,0x64,0xa4,
            0x56,0x49,0x59,0xee,0xd4,0x70,0x49,0xfc,
            0xe1,0x0d,0x0e,0xbb,0xe8,0x71,0xef,0x3d
        };
        return kExpected;
    }

    static bool FixedTimeEquals32(const uint8_t* a, const uint8_t* b) {
        uint8_t diff = 0;
        for (size_t i = 0; i < 32; ++i) diff |= (a[i] ^ b[i]);
        return diff == 0;
    }

    static bool Sha256(const void* data, size_t len, uint8_t out32[32]) {
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
        NTSTATUS st = BCryptHash(hAlg, nullptr, 0,
                                 const_cast<PUCHAR>(static_cast<const uint8_t*>(data)),
                                 static_cast<ULONG>(len), out32, 32);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return st == 0;
    }

    static std::wstring MarkerPath() {
        return DataPaths::Root() + L"\\activation-v1";
    }

    static bool IsActivated() {
        std::vector<uint8_t> bytes;
        if (!ReadAllBytesW(MarkerPath(), bytes) || bytes.size() != 32) return false;
        return FixedTimeEquals32(bytes.data(), ExpectedBytes());
    }

    static bool Activate(const std::string& keyUtf8, bool& outIoError) {
        outIoError = false;
        std::string trimmed = Trim(keyUtf8);
        uint8_t digest[32]{};
        if (!Sha256(trimmed.data(), trimmed.size(), digest)) {
            outIoError = true;
            return false;
        }
        if (!FixedTimeEquals32(digest, ExpectedBytes())) return false;
        DataPaths::Ensure();
        std::wstring marker = MarkerPath();
        std::wstring temp = marker + L".tmp";
        if (!WriteAllBytesW(temp, ExpectedBytes(), 32)) {
            outIoError = true;
            return false;
        }
        if (!MoveFileExW(temp.c_str(), marker.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp.c_str());
            outIoError = true;
            return false;
        }
        return true;
    }
};

class SingBoxProcess {
    HANDLE hProcess_ = nullptr;
    DWORD pid_ = 0;
    std::atomic<int> stopping_{0};

public:
    SingBoxProcess(HANDLE hProcess, DWORD pid) : hProcess_(hProcess), pid_(pid) {}
    ~SingBoxProcess() { Stop(); }

    SingBoxProcess(const SingBoxProcess&) = delete;
    SingBoxProcess& operator=(const SingBoxProcess&) = delete;

    DWORD ProcessId() const { return pid_; }

    bool HasExited() const {
        if (!hProcess_) return true;
        return WaitForSingleObject(hProcess_, 0) == WAIT_OBJECT_0;
    }

    static std::unique_ptr<SingBoxProcess> Start(const std::wstring& exePath, const std::wstring& configPath, std::string& outErr) {
        if (!FileExistsW(exePath)) {
            outErr = "sing-box.exe is missing from the installation folder.";
            return nullptr;
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        std::wstring cmd = L"\"" + exePath + L"\" run -c \"" + configPath + L"\"";
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');

        size_t slash = exePath.find_last_of(L"\\/");
        std::wstring workDir = (slash == std::wstring::npos) ? DataPaths::BaseDir() : exePath.substr(0, slash);

        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(
            exePath.c_str(),
            mutableCmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            workDir.c_str(),
            &si,
            &pi
        );
        if (!ok) {
            outErr = "Windows could not start the VPN engine. Check Windows security or the install folder.";
            return nullptr;
        }
        CloseHandle(pi.hThread);
        return std::make_unique<SingBoxProcess>(pi.hProcess, pi.dwProcessId);
    }

    void Stop() {
        if (stopping_.exchange(1) != 0) return;
        if (!hProcess_) return;
        if (!HasExited()) {
            static std::mutex consoleMtx;
            {
                std::lock_guard<std::mutex> lk(consoleMtx);
                BOOL attached = AttachConsole(pid_);
                BOOL ignored = FALSE;
                if (attached) {
                    ignored = SetConsoleCtrlHandler(nullptr, TRUE);
                    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid_);
                    FreeConsole();
                    if (ignored) SetConsoleCtrlHandler(nullptr, FALSE);
                }
            }
            if (WaitForSingleObject(hProcess_, 4500) != WAIT_OBJECT_0) {
                TerminateProcess(hProcess_, 1);
                WaitForSingleObject(hProcess_, 3000);
            }
        }
        CloseHandle(hProcess_);
        hProcess_ = nullptr;
    }
};

enum class ProbeState { Pending, Checking, Ok, Failed };

struct ProbeResult {
    bool success = false;
    double averageMs = 0.0;
    double score = 0.0;
    int samples = 0;
    std::string resolvedIp;
    std::string error;

    static ProbeResult Fail(const std::string& msg, const std::string& ip = "") {
        ProbeResult r;
        r.success = false;
        r.resolvedIp = ip;
        r.error = msg;
        return r;
    }
};

struct ServerEntry {
    ServerProfile profile;
    ProbeState state = ProbeState::Pending;
    double ms = 0.0;
    double score = 0.0;
    int samples = 0;
    std::string error;
    bool isActive = false;
    std::string resolvedIp;

    explicit ServerEntry(const ServerProfile& p) : profile(p) {}

    void SetPending() { state = ProbeState::Pending; ms = score = 0.0; samples = 0; error.clear(); }
    void SetChecking() { state = ProbeState::Checking; ms = score = 0.0; samples = 0; error.clear(); }
    void SetSuccess(double avgMs, int smp, double sc, const std::string& ip) {
        state = ProbeState::Ok; ms = avgMs; samples = smp; score = sc; error.clear(); resolvedIp = ip;
    }
    void SetFailure(const std::string& err) { state = ProbeState::Failed; error = err; }

    std::string ValueText() const {
        switch (state) {
            case ProbeState::Checking: return "Checking\xE2\x80\xA6";
            case ProbeState::Failed:   return "Unavailable";
            case ProbeState::Ok: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.0f ms", ms);
                return buf;
            }
            default: return "\xE2\x80\x94";
        }
    }

    std::string Subtitle() const {
        if (isActive) return "Active connection";
        switch (state) {
            case ProbeState::Checking: return "Testing the VMess connection";
            case ProbeState::Ok:       return samples > 1 ? "Verified \xC2\xB7 2 samples" : "Verified \xC2\xB7 1 sample";
            case ProbeState::Failed:   return "No verified response \xC2\xB7 tap for info";
            default:                   return "Not tested yet";
        }
    }
};

class ServerTester {
    static int NowMsRemaining(std::chrono::steady_clock::time_point deadline) {
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        return rem > 0 ? static_cast<int>(rem) : 0;
    }

    static bool SetSocketTimeouts(SOCKET s, int ms) {
        DWORD tv = static_cast<DWORD>(std::max(100, ms));
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        return true;
    }

    static bool SendExact(SOCKET s, const void* buf, int len, std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancel) {
        const char* p = static_cast<const char*>(buf);
        int sent = 0;
        while (sent < len) {
            if (cancel.load()) return false;
            int rem = NowMsRemaining(deadline);
            if (rem <= 0) return false;
            SetSocketTimeouts(s, rem);
            int n = send(s, p + sent, len - sent, 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    static bool RecvExact(SOCKET s, void* buf, int len, std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancel) {
        char* p = static_cast<char*>(buf);
        int got = 0;
        while (got < len) {
            if (cancel.load()) return false;
            int rem = NowMsRemaining(deadline);
            if (rem <= 0) return false;
            SetSocketTimeouts(s, rem);
            int n = recv(s, p + got, len - got, 0);
            if (n <= 0) return false;
            got += n;
        }
        return true;
    }

    static SOCKET ConnectLoopbackWithTimeout(int port, int timeoutMs) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
        u_long nonBlock = 1;
        ioctlsocket(s, FIONBIO, &nonBlock);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        if (rc != 0) {
            fd_set wfds, efds;
            FD_ZERO(&wfds); FD_SET(s, &wfds);
            FD_ZERO(&efds); FD_SET(s, &efds);
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            int sel = select(0, nullptr, &wfds, &efds, &tv);
            if (sel <= 0 || FD_ISSET(s, &efds) || !FD_ISSET(s, &wfds)) {
                closesocket(s);
                return INVALID_SOCKET;
            }
            int soErr = 0;
            int soLen = sizeof(soErr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen);
            if (soErr != 0) {
                closesocket(s);
                return INVALID_SOCKET;
            }
        }
        nonBlock = 0;
        ioctlsocket(s, FIONBIO, &nonBlock);
        return s;
    }

    static bool WaitForSocks(SingBoxProcess& core, int port, int timeoutMs, const std::atomic<bool>& cancel) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel.load() || core.HasExited()) return false;
            SOCKET s = ConnectLoopbackWithTimeout(port, 250);
            if (s != INVALID_SOCKET) {
                closesocket(s);
                return true;
            }
            for (int i = 0; i < 6 && !cancel.load(); ++i) Sleep(20);
        }
        return false;
    }

    static bool HttpsThroughSocks(int port, const std::string& targetHost, const std::string& path,
                                  int timeoutMs, const std::atomic<bool>& cancel,
                                  double& outElapsedMs, std::string& outErr) {
        auto start = std::chrono::steady_clock::now();
        auto deadline = start + std::chrono::milliseconds(timeoutMs);

        SOCKET s = ConnectLoopbackWithTimeout(port, std::min(timeoutMs, 2500));
        if (s == INVALID_SOCKET) {
            outErr = "The check endpoint or server is unreachable.";
            return false;
        }

        auto cleanupSocket = [&]() {
            if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
        };

        // SOCKS5 auth negotiation
        uint8_t helloReq[3] = {5, 1, 0};
        uint8_t helloRes[2] = {0, 0};
        if (!SendExact(s, helloReq, 3, deadline, cancel) ||
            !RecvExact(s, helloRes, 2, deadline, cancel) ||
            helloRes[0] != 5 || helloRes[1] != 0) {
            cleanupSocket();
            outErr = "The local SOCKS proxy rejected the check.";
            return false;
        }

        // SOCKS5 CONNECT domain:443
        if (targetHost.size() > 255) {
            cleanupSocket();
            outErr = "Probe host is too long.";
            return false;
        }
        std::vector<uint8_t> req(7 + targetHost.size());
        req[0] = 5; req[1] = 1; req[2] = 0; req[3] = 3;
        req[4] = static_cast<uint8_t>(targetHost.size());
        std::memcpy(req.data() + 5, targetHost.data(), targetHost.size());
        req[req.size() - 2] = 1;
        req[req.size() - 1] = 187; // 443

        uint8_t rep[4]{};
        if (!SendExact(s, req.data(), static_cast<int>(req.size()), deadline, cancel) ||
            !RecvExact(s, rep, 4, deadline, cancel) ||
            rep[0] != 5 || rep[1] != 0) {
            cleanupSocket();
            outErr = "The remote VMess proxy could not open the HTTPS check.";
            return false;
        }
        int rest = 0;
        if (rep[3] == 1) rest = 6;
        else if (rep[3] == 4) rest = 18;
        else if (rep[3] == 3) {
            uint8_t dlen = 0;
            if (!RecvExact(s, &dlen, 1, deadline, cancel)) { cleanupSocket(); outErr = "Invalid SOCKS response."; return false; }
            rest = static_cast<int>(dlen) + 2;
        } else {
            cleanupSocket();
            outErr = "Invalid SOCKS response.";
            return false;
        }
        std::vector<uint8_t> tail(static_cast<size_t>(rest));
        if (!RecvExact(s, tail.data(), rest, deadline, cancel)) {
            cleanupSocket();
            outErr = "Invalid SOCKS response.";
            return false;
        }

        // Schannel TLS 1.2 / 1.3 handshake with Windows certificate verification
        SCHANNEL_CRED cred{};
        cred.dwVersion = SCHANNEL_CRED_VERSION;
        cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
        cred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;

        CredHandle hCred{};
        TimeStamp tsExpiry{};
        SECURITY_STATUS ss = AcquireCredentialsHandleW(
            nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
            nullptr, &cred, nullptr, nullptr, &hCred, &tsExpiry);
        if (ss != SEC_E_OK) {
            cleanupSocket();
            outErr = "The TLS certificate check failed.";
            return false;
        }

        CtxtHandle hCtx{};
        bool haveCtx = false;
        std::wstring wideHost = ToWide(targetHost);
        ULONG ctxReq = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                       ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

        std::vector<char> ioBuf(65536);
        DWORD ioLen = 0;
        bool handshakeOk = false;

        while (!cancel.load() && std::chrono::steady_clock::now() < deadline) {
            SecBuffer inBufs[2]{};
            inBufs[0].pvBuffer = ioBuf.data();
            inBufs[0].cbBuffer = ioLen;
            inBufs[0].BufferType = SECBUFFER_TOKEN;
            inBufs[1].pvBuffer = nullptr;
            inBufs[1].cbBuffer = 0;
            inBufs[1].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc inDesc{SECBUFFER_VERSION, 2, inBufs};

            SecBuffer outBuf{};
            outBuf.pvBuffer = nullptr;
            outBuf.cbBuffer = 0;
            outBuf.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc outDesc{SECBUFFER_VERSION, 1, &outBuf};

            ULONG ctxAttr = 0;
            ss = InitializeSecurityContextW(
                &hCred,
                haveCtx ? &hCtx : nullptr,
                const_cast<SEC_WCHAR*>(wideHost.c_str()),
                ctxReq, 0, 0,
                haveCtx ? &inDesc : nullptr,
                0,
                haveCtx ? nullptr : &hCtx,
                &outDesc,
                &ctxAttr,
                &tsExpiry
            );
            if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED || FAILED(ss) == false) {
                haveCtx = true;
            }

            if (outBuf.pvBuffer && outBuf.cbBuffer > 0) {
                bool sentOk = SendExact(s, outBuf.pvBuffer, static_cast<int>(outBuf.cbBuffer), deadline, cancel);
                FreeContextBuffer(outBuf.pvBuffer);
                if (!sentOk) break;
            }

            if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                int rem = NowMsRemaining(deadline);
                if (rem <= 0) break;
                SetSocketTimeouts(s, rem);
                if (ioLen >= ioBuf.size()) ioBuf.resize(ioBuf.size() * 2);
                int n = recv(s, ioBuf.data() + ioLen, static_cast<int>(ioBuf.size() - ioLen), 0);
                if (n <= 0) break;
                ioLen += static_cast<DWORD>(n);
                continue;
            }

            // Preserve any extra trailing bytes
            if (inBufs[1].BufferType == SECBUFFER_EXTRA && inBufs[1].cbBuffer > 0) {
                std::memmove(ioBuf.data(), ioBuf.data() + (ioLen - inBufs[1].cbBuffer), inBufs[1].cbBuffer);
                ioLen = inBufs[1].cbBuffer;
            } else if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                ioLen = 0;
            }

            if (ss == SEC_E_OK) {
                handshakeOk = true;
                break;
            }
            if (ss == SEC_I_CONTINUE_NEEDED) {
                if (ioLen == 0) {
                    int rem = NowMsRemaining(deadline);
                    if (rem <= 0) break;
                    SetSocketTimeouts(s, rem);
                    int n = recv(s, ioBuf.data(), static_cast<int>(ioBuf.size()), 0);
                    if (n <= 0) break;
                    ioLen = static_cast<DWORD>(n);
                }
                continue;
            }
            // Certificate or protocol failure
            outErr = "The TLS certificate check failed.";
            break;
        }

        if (!handshakeOk) {
            if (haveCtx) DeleteSecurityContext(&hCtx);
            FreeCredentialsHandle(&hCred);
            cleanupSocket();
            if (outErr.empty()) outErr = "No verified response (offline, blocked endpoint, or timeout).";
            return false;
        }

        SecPkgContext_StreamSizes sizes{};
        if (QueryContextAttributesW(&hCtx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
            DeleteSecurityContext(&hCtx);
            FreeCredentialsHandle(&hCred);
            cleanupSocket();
            outErr = "TLS stream setup failed.";
            return false;
        }

        std::string httpReq = "GET " + path + " HTTP/1.1\r\nHost: " + targetHost +
                              "\r\nUser-Agent: VPN Echo/1.1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
        std::vector<char> encBuf(sizes.cbHeader + httpReq.size() + sizes.cbTrailer);
        std::memcpy(encBuf.data() + sizes.cbHeader, httpReq.data(), httpReq.size());

        SecBuffer eBufs[4]{};
        eBufs[0].pvBuffer = encBuf.data();
        eBufs[0].cbBuffer = sizes.cbHeader;
        eBufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        eBufs[1].pvBuffer = encBuf.data() + sizes.cbHeader;
        eBufs[1].cbBuffer = static_cast<ULONG>(httpReq.size());
        eBufs[1].BufferType = SECBUFFER_DATA;
        eBufs[2].pvBuffer = encBuf.data() + sizes.cbHeader + httpReq.size();
        eBufs[2].cbBuffer = sizes.cbTrailer;
        eBufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        eBufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc eDesc{SECBUFFER_VERSION, 4, eBufs};

        if (EncryptMessage(&hCtx, 0, &eDesc, 0) != SEC_E_OK ||
            !SendExact(s, encBuf.data(), static_cast<int>(eBufs[0].cbBuffer + eBufs[1].cbBuffer + eBufs[2].cbBuffer), deadline, cancel)) {
            DeleteSecurityContext(&hCtx);
            FreeCredentialsHandle(&hCred);
            cleanupSocket();
            outErr = "Failed to send HTTPS probe request.";
            return false;
        }

        std::string plainResp;
        while (!cancel.load() && plainResp.find("\r\n\r\n") == std::string::npos && plainResp.size() < 8192) {
            if (ioLen == 0) {
                int rem = NowMsRemaining(deadline);
                if (rem <= 0) break;
                SetSocketTimeouts(s, rem);
                int n = recv(s, ioBuf.data(), static_cast<int>(ioBuf.size()), 0);
                if (n <= 0) break;
                ioLen = static_cast<DWORD>(n);
            }
            SecBuffer dBufs[4]{};
            dBufs[0].pvBuffer = ioBuf.data();
            dBufs[0].cbBuffer = ioLen;
            dBufs[0].BufferType = SECBUFFER_DATA;
            dBufs[1].BufferType = SECBUFFER_EMPTY;
            dBufs[2].BufferType = SECBUFFER_EMPTY;
            dBufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc dDesc{SECBUFFER_VERSION, 4, dBufs};

            ss = DecryptMessage(&hCtx, &dDesc, 0, nullptr);
            if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                int rem = NowMsRemaining(deadline);
                if (rem <= 0) break;
                SetSocketTimeouts(s, rem);
                if (ioLen >= ioBuf.size()) ioBuf.resize(ioBuf.size() * 2);
                int n = recv(s, ioBuf.data() + ioLen, static_cast<int>(ioBuf.size() - ioLen), 0);
                if (n <= 0) break;
                ioLen += static_cast<DWORD>(n);
                continue;
            }
            if (ss != SEC_E_OK && ss != SEC_I_CONTEXT_EXPIRED && ss != SEC_I_RENEGOTIATE) break;

            SecBuffer* extra = nullptr;
            for (int i = 0; i < 4; ++i) {
                if (dBufs[i].BufferType == SECBUFFER_DATA && dBufs[i].cbBuffer > 0) {
                    plainResp.append(static_cast<const char*>(dBufs[i].pvBuffer), dBufs[i].cbBuffer);
                } else if (dBufs[i].BufferType == SECBUFFER_EXTRA && dBufs[i].cbBuffer > 0) {
                    extra = &dBufs[i];
                }
            }
            if (extra) {
                std::memmove(ioBuf.data(), extra->pvBuffer, extra->cbBuffer);
                ioLen = extra->cbBuffer;
            } else {
                ioLen = 0;
            }
            if (ss == SEC_I_CONTEXT_EXPIRED) break;
        }

        DeleteSecurityContext(&hCtx);
        FreeCredentialsHandle(&hCred);
        cleanupSocket();

        size_t eol = plainResp.find("\r\n");
        std::string statusLine = (eol == std::string::npos) ? plainResp : plainResp.substr(0, eol);
        if (statusLine.find(" 204 ") == std::string::npos &&
            !(statusLine.size() >= 4 && statusLine.compare(statusLine.size() - 4, 4, " 204") == 0)) {
            outErr = "The HTTPS test endpoint did not return 204.";
            return false;
        }

        outElapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return true;
    }

public:
    static bool ResolveIpv4(const std::string& host, std::string& outIp) {
        ADDRINFOW hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        PADDRINFOW res = nullptr;
        std::wstring whost = ToWide(host);
        if (GetAddrInfoW(whost.c_str(), nullptr, &hints, &res) != 0 || !res) {
            return false;
        }
        char buf[64]{};
        sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
        InetNtopA(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        FreeAddrInfoW(res);
        outIp = buf;
        return !outIp.empty();
    }

    static int GetFreePort() {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return 0;
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sin.sin_port = 0;
        if (bind(s, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) != 0 || listen(s, 1) != 0) {
            closesocket(s);
            return 0;
        }
        int len = sizeof(sin);
        getsockname(s, reinterpret_cast<sockaddr*>(&sin), &len);
        int port = ntohs(sin.sin_port);
        closesocket(s);
        return port;
    }

    static ProbeResult Test(const ServerProfile& profile, const std::atomic<bool>& cancel) {
        DataPaths::Ensure();
        std::wstring coreExe = DataPaths::CoreExe();
        if (!FileExistsW(coreExe)) {
            return ProbeResult::Fail("The bundled VPN engine is missing.");
        }
        std::string ip;
        if (!ResolveIpv4(profile.address, ip)) {
            return ProbeResult::Fail("The server name could not be resolved to IPv4.");
        }
        if (cancel.load()) return ProbeResult::Fail("Check cancelled.", ip);

        int port = GetFreePort();
        if (port <= 0) return ProbeResult::Fail("Could not allocate local probe port.", ip);

        std::wstring configPath = DataPaths::UniqueConfig(L"probe", profile.id);
        std::wstring logPath = DataPaths::UniqueLog(L"probe", profile.id);
        std::string cfgJson = VpnConfigFactory::CreateProbe(profile, ip, port, ToUtf8(logPath));
        if (!WriteAllBytesW(configPath, cfgJson.data(), cfgJson.size())) {
            return ProbeResult::Fail("Could not write temporary probe configuration.", ip);
        }

        std::string startErr;
        auto core = SingBoxProcess::Start(coreExe, configPath, startErr);
        if (!core) {
            DeleteFileW(configPath.c_str());
            return ProbeResult::Fail(startErr, ip);
        }

        ProbeResult finalRes;
        if (!WaitForSocks(*core, port, 6000, cancel)) {
            finalRes = ProbeResult::Fail(
                cancel.load() ? "Check cancelled." :
                (core->HasExited() ? "VPN engine rejected this profile. Tap for details." : "No verified response before timeout."),
                ip);
        } else {
            double sample1 = 0.0;
            std::string err1;
            std::string workingHost;
            if (HttpsThroughSocks(port, "www.gstatic.com", "/generate_204", 5000, cancel, sample1, err1)) {
                workingHost = "www.gstatic.com";
            } else if (!cancel.load() && HttpsThroughSocks(port, "cp.cloudflare.com", "/generate_204", 5000, cancel, sample1, err1)) {
                workingHost = "cp.cloudflare.com";
            }

            if (workingHost.empty()) {
                finalRes = ProbeResult::Fail(cancel.load() ? "Check cancelled." : (err1.empty() ? "No verified response (offline, blocked endpoint, or timeout)." : err1), ip);
            } else {
                int samples = 1;
                double average = sample1;
                double score = sample1 + 300.0;
                double sample2 = 0.0;
                std::string err2;
                if (!cancel.load() && HttpsThroughSocks(port, workingHost, "/generate_204", 3500, cancel, sample2, err2)) {
                    samples = 2;
                    average = (sample1 + sample2) / 2.0;
                    score = average + std::fabs(sample1 - sample2) * 0.2;
                }
                finalRes.success = true;
                finalRes.averageMs = average;
                finalRes.score = score;
                finalRes.samples = samples;
                finalRes.resolvedIp = ip;
            }
        }

        core->Stop();
        DeleteFileW(configPath.c_str());
        return finalRes;
    }
};

class VpnController {
    mutable std::mutex mtx_;
    std::vector<ServerEntry> entries_;
    std::string state_ = "Disconnected";
    std::string message_;
    int completed_ = 0;
    int revision_ = 0;
    int activeId_ = -1;
    std::string activeName_ = "Automatic";
    std::atomic<long long> downloadRate_{0};
    std::atomic<long long> uploadRate_{0};

    std::unique_ptr<SingBoxProcess> core_;
    std::mutex coreGate_;
    std::shared_ptr<std::atomic<bool>> currentOpCancel_;

    std::chrono::steady_clock::time_point nextHealth_ = std::chrono::steady_clock::time_point::max();
    int healthFailures_ = 0;
    std::atomic<int> tickRunning_{0};
    bool hasTrafficBaseline_ = false;
    std::chrono::steady_clock::time_point lastStatsAt_{};
    uint64_t lastReceived_ = 0;
    uint64_t lastSent_ = 0;

    std::function<void()> onChanged_;

    void Notify() {
        if (onChanged_) onChanged_();
    }

    static bool CheckDirect204(const wchar_t* host, const wchar_t* path) {
        HINTERNET hSession = WinHttpOpen(L"VPN Echo/1.1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;
        WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);

        HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }
        DWORD disableRedirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disableRedirect, sizeof(disableRedirect));

        DWORD statusCode = 0;
        DWORD sz = sizeof(statusCode);
        bool ok = false;
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, nullptr) &&
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX)) {
            ok = (statusCode == 204);
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return ok;
    }

    static bool VerifySystemTunnel(SingBoxProcess& core, int maxSeconds, const std::atomic<bool>& cancel) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 15 && !cancel.load(); ++i) Sleep(50); // 750ms initial settle
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(maxSeconds)) {
            if (cancel.load() || core.HasExited()) return false;
            if (CheckDirect204(L"www.gstatic.com", L"/generate_204")) return true;
            if (cancel.load() || core.HasExited()) return false;
            if (CheckDirect204(L"cp.cloudflare.com", L"/generate_204")) return true;
            for (int i = 0; i < 17 && !cancel.load(); ++i) Sleep(50); // 850ms
        }
        return false;
    }

    static bool HasLogTail(const std::wstring& path) {
        std::vector<uint8_t> data;
        if (!ReadAllBytesW(path, data) || data.empty()) return false;
        std::string s(reinterpret_cast<const char*>(data.data()), data.size());
        return !Trim(s).empty();
    }

    bool TryConnectCandidate(int entryId, const std::atomic<bool>& cancel) {
        ServerProfile profile;
        std::string resolvedIp;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& e : entries_) {
                if (e.profile.id == entryId) {
                    profile = e.profile;
                    resolvedIp = e.resolvedIp;
                    break;
                }
            }
            state_ = "Connecting";
            message_ = "Automatic \xC2\xB7 " + profile.name + " \xC2\xB7 verifying VPN connection\xE2\x80\xA6";
            ++revision_;
        }
        Notify();

        std::lock_guard<std::mutex> gate(coreGate_);
        if (core_) { core_->Stop(); core_.reset(); }
        if (cancel.load()) return false;

        if (resolvedIp.empty() && !ServerTester::ResolveIpv4(profile.address, resolvedIp)) {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& e : entries_) {
                if (e.profile.id == entryId) {
                    e.SetFailure("The server name could not be resolved to IPv4.");
                    e.isActive = false;
                }
            }
            ++revision_;
            Notify();
            return false;
        }

        std::wstring logPath = DataPaths::UniqueLog(L"tun", profile.id);
        std::wstring cfgPath = DataPaths::UniqueConfig(L"tun", profile.id);
        std::string cfgJson = VpnConfigFactory::CreateTun(profile, resolvedIp, ToUtf8(logPath));
        WriteAllBytesW(cfgPath, cfgJson.data(), cfgJson.size());

        std::string startErr;
        core_ = SingBoxProcess::Start(DataPaths::CoreExe(), cfgPath, startErr);
        if (!core_) {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& e : entries_) {
                if (e.profile.id == entryId) { e.SetFailure(startErr); e.isActive = false; }
            }
            ++revision_;
            Notify();
            return false;
        }

        bool passed = VerifySystemTunnel(*core_, 14, cancel);
        if (!passed) {
            core_->Stop();
            core_.reset();
            if (!cancel.load()) {
                bool hasDetail = HasLogTail(logPath);
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& e : entries_) {
                    if (e.profile.id == entryId) {
                        e.SetFailure(hasDetail ? "Engine or tunnel check failed \xC2\xB7 tap for info"
                                               : "VPN verification failed \xC2\xB7 tap for info");
                        e.isActive = false;
                    }
                }
                ++revision_;
                Notify();
            }
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            activeId_ = profile.id;
            activeName_ = profile.name;
            healthFailures_ = 0;
            nextHealth_ = std::chrono::steady_clock::now() + std::chrono::seconds(25);
            hasTrafficBaseline_ = false;
            lastReceived_ = 0;
            lastSent_ = 0;
            downloadRate_.store(0);
            uploadRate_.store(0);
            for (auto& e : entries_) e.isActive = (e.profile.id == profile.id);
            state_ = "Connected";
            message_.clear();
            ++revision_;
        }
        Notify();
        return true;
    }

public:
    explicit VpnController(std::function<void()> onChanged) : onChanged_(std::move(onChanged)) {
        DataPaths::Ensure();
        std::vector<uint8_t> raw;
        if (!ReadAllBytesW(DataPaths::ServersJson(), raw)) {
            throw std::runtime_error("servers.json is missing from the application folder.");
        }
        std::string jsonText(reinterpret_cast<const char*>(raw.data()), raw.size());
        auto profiles = ServerProfile::LoadFromJsonText(jsonText);
        for (const auto& p : profiles) entries_.emplace_back(p);
    }

    ~VpnController() {
        DisconnectSync();
    }

    struct Snapshot {
        std::string state;
        std::string message;
        int completed = 0;
        int total = 3;
        int available = 0;
        int revision = 0;
        int activeId = -1;
        std::string activeName;
        long long downloadRate = 0;
        long long uploadRate = 0;
        std::vector<ServerEntry> ranked;
    };

    Snapshot GetSnapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        Snapshot s;
        s.state = state_;
        s.message = message_;
        s.completed = completed_;
        s.total = static_cast<int>(entries_.size());
        s.available = 0;
        for (const auto& e : entries_) {
            if (e.state == ProbeState::Ok) ++s.available;
        }
        s.revision = revision_;
        s.activeId = activeId_;
        s.activeName = activeName_;
        s.downloadRate = downloadRate_.load();
        s.uploadRate = uploadRate_.load();
        s.ranked = entries_;
        std::sort(s.ranked.begin(), s.ranked.end(), [](const ServerEntry& a, const ServerEntry& b) {
            int oa = (a.state == ProbeState::Ok) ? 0 : 1;
            int ob = (b.state == ProbeState::Ok) ? 0 : 1;
            if (oa != ob) return oa < ob;
            double sa = (a.state == ProbeState::Ok) ? a.score : 1e30;
            double sb = (b.state == ProbeState::Ok) ? b.score : 1e30;
            if (sa != sb) return sa < sb;
            return a.profile.id < b.profile.id;
        });
        return s;
    }

    void ToggleAsync() {
        std::string st;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            st = state_;
        }
        if (st != "Disconnected") {
            std::thread([this]() { DisconnectSync(); }).detach();
        } else {
            ConnectAsync();
        }
    }

    void ConnectAsync() {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (state_ != "Disconnected") return;
            currentOpCancel_ = cancel;
            state_ = "Checking";
            message_ = "Testing all three servers before connecting\xE2\x80\xA6";
            completed_ = 0;
            for (auto& e : entries_) {
                e.isActive = false;
                e.SetPending();
                e.SetChecking();
            }
            ++revision_;
        }
        Notify();

        std::thread([this, cancel]() {
            auto roundCancel = std::make_shared<std::atomic<bool>>(false);
            std::atomic<bool> roundDone{false};

            std::thread watchdog([cancel, roundCancel, &roundDone]() {
                for (int i = 0; i < 200; ++i) { // 20s round limit
                    if (roundDone.load() || cancel->load()) {
                        if (cancel->load()) roundCancel->store(true);
                        return;
                    }
                    Sleep(100);
                }
                roundCancel->store(true);
            });

            std::vector<ServerProfile> profs;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& e : entries_) profs.push_back(e.profile);
            }

            std::vector<std::thread> workers;
            for (const auto& p : profs) {
                workers.emplace_back([this, p, cancel, roundCancel]() {
                    ProbeResult res = ServerTester::Test(p, *roundCancel);
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        for (auto& e : entries_) {
                            if (e.profile.id == p.id) {
                                if (res.success) e.SetSuccess(res.averageMs, res.samples, res.score, res.resolvedIp);
                                else if (cancel->load()) e.SetFailure("Check cancelled.");
                                else e.SetFailure(res.error);
                            }
                        }
                        ++completed_;
                        ++revision_;
                        if (state_ == "Checking") {
                            message_ = "Checking servers \xC2\xB7 " + std::to_string(completed_) + "/" + std::to_string(entries_.size()) + " complete\xE2\x80\xA6";
                        }
                    }
                    Notify();
                });
            }
            for (auto& w : workers) w.join();
            roundDone.store(true);
            watchdog.join();

            if (cancel->load()) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (state_ != "Disconnecting") {
                    state_ = "Disconnected";
                    message_ = "Server checks cancelled.";
                    ++revision_;
                }
                if (currentOpCancel_ == cancel) currentOpCancel_.reset();
                Notify();
                return;
            }

            std::vector<int> candidates;
            {
                auto snap = GetSnapshot();
                for (const auto& e : snap.ranked) {
                    if (e.state == ProbeState::Ok) candidates.push_back(e.profile.id);
                }
            }
            if (candidates.empty()) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    state_ = "Disconnected";
                    message_ = "None of the three servers returned a verified HTTPS response. Check your network and try again.";
                    ++revision_;
                    if (currentOpCancel_ == cancel) currentOpCancel_.reset();
                }
                Notify();
                return;
            }

            for (int cid : candidates) {
                if (cancel->load()) break;
                if (TryConnectCandidate(cid, *cancel)) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (currentOpCancel_ == cancel) currentOpCancel_.reset();
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (!cancel->load() && state_ != "Disconnecting") {
                    state_ = "Disconnected";
                    message_ = "No verified server connected. Tap Connect to test again.";
                    ++revision_;
                }
                if (currentOpCancel_ == cancel) currentOpCancel_.reset();
            }
            Notify();
        }).detach();
    }

    void DisconnectSync() {
        std::shared_ptr<std::atomic<bool>> op;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            op = currentOpCancel_;
            currentOpCancel_.reset();
            if (state_ == "Disconnected" && !core_) return;
            state_ = "Disconnecting";
            message_ = "Stopping the VPN tunnel\xE2\x80\xA6";
            ++revision_;
        }
        if (op) op->store(true);
        Notify();

        {
            std::lock_guard<std::mutex> gate(coreGate_);
            if (core_) {
                core_->Stop();
                core_.reset();
            }
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            activeId_ = -1;
            activeName_ = "Automatic";
            nextHealth_ = std::chrono::steady_clock::time_point::max();
            healthFailures_ = 0;
            hasTrafficBaseline_ = false;
            downloadRate_.store(0);
            uploadRate_.store(0);
            for (auto& e : entries_) e.isActive = false;
            state_ = "Disconnected";
            message_ = "Disconnected.";
            ++revision_;
        }
        Notify();
    }

    void SampleTraffic() {
        std::string st;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            st = state_;
            if (st != "Connected" && st != "Reconnecting") {
                hasTrafficBaseline_ = false;
                return;
            }
        }

        PMIB_IF_TABLE2 table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR || !table) return;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            std::string alias = ToUtf8(row.Alias);
            std::string desc = ToUtf8(row.Description);
            if (ContainsIgnoreCase(alias, "VPN Echo") || ContainsIgnoreCase(desc, "VPN Echo")) {
                auto now = std::chrono::steady_clock::now();
                uint64_t rx = row.InOctets;
                uint64_t tx = row.OutOctets;
                std::lock_guard<std::mutex> lk(mtx_);
                if (hasTrafficBaseline_) {
                    double sec = std::max(0.1, std::chrono::duration<double>(now - lastStatsAt_).count());
                    long long dr = (rx >= lastReceived_) ? static_cast<long long>((rx - lastReceived_) / sec) : 0;
                    long long ur = (tx >= lastSent_) ? static_cast<long long>((tx - lastSent_) / sec) : 0;
                    downloadRate_.store(dr);
                    uploadRate_.store(ur);
                }
                hasTrafficBaseline_ = true;
                lastStatsAt_ = now;
                lastReceived_ = rx;
                lastSent_ = tx;
                break;
            }
        }
        FreeMibTable(table);
    }

    void TickFromTimer() {
        if (tickRunning_.exchange(1) != 0) return;
        std::thread([this]() {
            SampleTraffic();
            bool shouldHealthCheck = false;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if ((state_ == "Connected" || state_ == "Reconnecting") &&
                    std::chrono::steady_clock::now() >= nextHealth_) {
                    shouldHealthCheck = true;
                }
            }
            if (shouldHealthCheck) {
                bool healthy = false;
                {
                    std::lock_guard<std::mutex> gate(coreGate_);
                    if (core_ && !core_->HasExited()) {
                        std::atomic<bool> hcCancel{false};
                        healthy = VerifySystemTunnel(*core_, 8, hcCancel);
                    }
                }
                int failoverFromId = -1;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (state_ == "Connected" || state_ == "Reconnecting") {
                        if (healthy) {
                            healthFailures_ = 0;
                            nextHealth_ = std::chrono::steady_clock::now() + std::chrono::seconds(25);
                            if (state_ == "Reconnecting") {
                                state_ = "Connected";
                                message_.clear();
                                ++revision_;
                            }
                        } else if (++healthFailures_ == 1) {
                            nextHealth_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                            state_ = "Reconnecting";
                            message_ = "Connection interrupted \xC2\xB7 checking again\xE2\x80\xA6";
                            ++revision_;
                        } else {
                            nextHealth_ = std::chrono::steady_clock::now() + std::chrono::seconds(25);
                            failoverFromId = activeId_;
                        }
                    }
                }
                Notify();

                if (failoverFromId >= 0) {
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        for (auto& e : entries_) {
                            if (e.profile.id == failoverFromId) {
                                e.SetFailure("Connection was lost.");
                                e.isActive = false;
                            }
                        }
                        state_ = "Reconnecting";
                        message_ = "Automatic \xC2\xB7 trying another verified server\xE2\x80\xA6";
                        ++revision_;
                    }
                    Notify();

                    std::vector<int> backups;
                    for (const auto& e : GetSnapshot().ranked) {
                        if (e.state == ProbeState::Ok && e.profile.id != failoverFromId) {
                            backups.push_back(e.profile.id);
                        }
                    }
                    std::atomic<bool> foCancel{false};
                    bool recovered = false;
                    for (int bid : backups) {
                        if (TryConnectCandidate(bid, foCancel)) { recovered = true; break; }
                    }
                    if (!recovered) {
                        DisconnectSync();
                        std::lock_guard<std::mutex> lk(mtx_);
                        state_ = "Disconnected";
                        message_ = "Connection lost. No other checked server could connect. Tap Connect to run fresh checks.";
                        ++revision_;
                        Notify();
                    }
                }
            }
            tickRunning_.store(0);
        }).detach();
    }
};

} // namespace vpnecho
