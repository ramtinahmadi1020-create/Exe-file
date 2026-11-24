#include "vpn_engine.hpp"
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#define IDI_APPICON 101

namespace {

using namespace vpnecho;
using namespace Gdiplus;

constexpr UINT WM_APP_CHANGED   = WM_APP + 1;
constexpr UINT WM_APP_TRAYICON  = WM_APP + 2;
constexpr UINT TIMER_RENDER     = 1;
constexpr UINT TIMER_TRAFFIC    = 2;
constexpr UINT TIMER_PROGRESS   = 3;

constexpr UINT ID_TRAY_OPEN       = 1001;
constexpr UINT ID_TRAY_DISCONNECT = 1002;
constexpr UINT ID_TRAY_EXIT       = 1003;
constexpr int  IDC_LICENSE_EDIT   = 2001;

enum class HitZone {
    None,
    MinimizeBtn,
    CloseBtn,
    ActivateBtn,
    PowerBtn,
    ServerCard0,
    ServerCard1,
    ServerCard2,
    CreditArea
};

void AddRoundedRect(GraphicsPath& path, const RectF& r, REAL radius) {
    if (radius <= 0.1f) {
        path.AddRectangle(r);
        return;
    }
    REAL d = std::min({radius * 2.0f, r.Width, r.Height});
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(Graphics& g, const Brush* b, const RectF& r, REAL radius) {
    GraphicsPath path;
    AddRoundedRect(path, r, radius);
    g.FillPath(b, &path);
}

void DrawRoundRect(Graphics& g, const Pen* p, const RectF& r, REAL radius) {
    GraphicsPath path;
    AddRoundedRect(path, r, radius);
    g.DrawPath(p, &path);
}

class MainWindowApp {
    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND hEdit_ = nullptr;
    WNDPROC origEditProc_ = nullptr;
    HBRUSH editBgBrush_ = nullptr;
    HFONT editFont_ = nullptr;
    HICON hAppIcon_ = nullptr;
    HICON hSmallIcon_ = nullptr;

    float dpiScale_ = 1.0f;
    bool activated_ = false;
    bool allowClose_ = false;
    std::wstring licenseError_;

    HitZone hoverZone_ = HitZone::None;
    HitZone pressZone_ = HitZone::None;
    float progressPhase_ = 0.0f;

    NOTIFYICONDATAW nid_{};
    bool trayAdded_ = false;

    std::unique_ptr<VpnController> controller_;
    VpnController::Snapshot snap_;

    float S(float v) const { return v * dpiScale_; }
    int   SI(float v) const { return static_cast<int>(std::lround(v * dpiScale_)); }

    RectF MinimizeRect() const { return RectF(S(341.0f), S(12.0f), S(34.0f), S(34.0f)); }
    RectF CloseRect()    const { return RectF(S(379.0f), S(12.0f), S(34.0f), S(34.0f)); }
    RectF ActivateRect() const { return RectF(S(28.0f),  S(462.0f), S(364.0f), S(54.0f)); }
    RectF InputBoxRect() const { return RectF(S(28.0f),  S(368.0f), S(364.0f), S(56.0f)); }
    RectF PowerRect()    const { return RectF(S(115.0f), S(64.0f),  S(190.0f), S(190.0f)); }
    RectF CardRect(int idx) const {
        return RectF(S(20.0f), S(366.0f + idx * 76.0f), S(380.0f), S(68.0f));
    }
    RectF CreditRect()   const { return RectF(S(240.0f), S(806.0f), S(165.0f), S(32.0f)); }

    static bool PtInCircle(float x, float y, const RectF& r) {
        float cx = r.X + r.Width * 0.5f;
        float cy = r.Y + r.Height * 0.5f;
        float rx = r.Width * 0.5f;
        float dx = x - cx, dy = y - cy;
        return (dx * dx + dy * dy) <= (rx * rx);
    }

    HitZone HitTestControls(float x, float y) const {
        if (MinimizeRect().Contains(x, y)) return HitZone::MinimizeBtn;
        if (CloseRect().Contains(x, y))    return HitZone::CloseBtn;
        if (CreditRect().Contains(x, y))   return HitZone::CreditArea;
        if (!activated_) {
            if (ActivateRect().Contains(x, y)) return HitZone::ActivateBtn;
        } else {
            if (PtInCircle(x, y, PowerRect())) return HitZone::PowerBtn;
            for (int i = 0; i < static_cast<int>(snap_.ranked.size()) && i < 3; ++i) {
                if (CardRect(i).Contains(x, y)) {
                    return static_cast<HitZone>(static_cast<int>(HitZone::ServerCard0) + i);
                }
            }
        }
        return HitZone::None;
    }

    void UpdateEditFontAndBounds() {
        if (!hEdit_) return;
        if (editFont_) { DeleteObject(editFont_); editFont_ = nullptr; }
        editFont_ = CreateFontW(
            -SI(16.0f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SendMessageW(hEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
        SetWindowPos(hEdit_, nullptr, SI(44.0f), SI(385.0f), SI(332.0f), SI(24.0f),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hEdit_, activated_ ? SW_HIDE : SW_SHOW);
    }

    void RefreshSnapshotAndTray() {
        if (!controller_) return;
        snap_ = controller_->GetSnapshot();
        if (trayAdded_) {
            std::string tipUtf8 = "VPN Echo \xC2\xB7 " + snap_.state + " \xC2\xB7 " + snap_.activeName +
                                  " \xC2\xB7 \xE2\x86\x93" + VpnConfigFactory::FormatRate(snap_.downloadRate) +
                                  " \xE2\x86\x91" + VpnConfigFactory::FormatRate(snap_.uploadRate);
            std::wstring tip = ToWide(tipUtf8);
            if (tip.size() > 63) tip.resize(63);
            std::wcsncpy(nid_.szTip, tip.c_str(), 127);
            nid_.szTip[127] = L'\0';
            nid_.uFlags = NIF_TIP;
            Shell_NotifyIconW(NIM_MODIFY, &nid_);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void TryActivate() {
        if (!hEdit_) return;
        wchar_t buf[512]{};
        GetWindowTextW(hEdit_, buf, 511);
        bool ioErr = false;
        if (!LicenseStore::Activate(ToUtf8(buf), ioErr)) {
            licenseError_ = ioErr ? L"Could not save activation. Please try again."
                                  : L"Invalid license. Please try again.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        licenseError_.clear();
        SetWindowTextW(hEdit_, L"");
        activated_ = true;
        ShowWindow(hEdit_, SW_HIDE);
        RefreshSnapshotAndTray();
    }

    void ShowServerDetails(int idx) {
        if (idx < 0 || idx >= static_cast<int>(snap_.ranked.size())) return;
        const ServerEntry& entry = snap_.ranked[idx];
        std::wstring title = ToWide(entry.profile.name);
        std::wstring msg;
        UINT flags = MB_OK | MB_ICONINFORMATION;
        switch (entry.state) {
            case ProbeState::Failed:
                msg = entry.error.empty()
                    ? L"No verified response. Check the internet connection and try Connect again."
                    : ToWide(entry.error);
                flags = MB_OK | MB_ICONWARNING;
                break;
            case ProbeState::Ok: {
                wchar_t buf[512];
                std::swprintf(buf, 512,
                    L"HTTPS response latency: %.0f ms\nSuccessful samples: %d\n\n"
                    L"Green < 500 ms \u00B7 Orange 500\u20131500 ms \u00B7 Red > 1500 ms\n\n"
                    L"Connect checks all three profiles, then automatically uses the best verified one. Latency is not download bandwidth.",
                    entry.ms, entry.samples);
                msg = buf;
                break;
            }
            case ProbeState::Checking:
                msg = L"Echo is testing this VMess profile now.";
                break;
            default:
                msg = L"Tap Connect to test all three servers. Echo will display the results and choose the best verified server automatically.";
                break;
        }
        MessageBoxW(hwnd_, msg.c_str(), title.c_str(), flags);
    }

    void ShowAboutDialog() {
        MessageBoxW(
            hwnd_,
            L"VPN Echo 1.1.0 \x00B7 developer: Ramtin Ahmadi\n\n"
            L"The Windows tunnel uses the unmodified sing-box 1.14.4 core. GPL-3.0 license and corresponding source: https://github.com/SagerNet/sing-box/tree/v1.14.4\n\n"
            L"Connection checks verify HTTPS through each profile; availability depends on the server and your network.",
            L"About VPN Echo",
            MB_OK | MB_ICONINFORMATION
        );
    }

    void ShowMainWindow() {
        ShowWindow(hwnd_, SW_SHOW);
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
    }

    void ShowTrayMenu() {
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return;
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"Open VPN Echo");
        UINT discFlags = MF_STRING | ((snap_.state == "Disconnected" || snap_.state == "Disconnecting") ? MF_GRAYED : MF_ENABLED);
        AppendMenuW(hMenu, discFlags, ID_TRAY_DISCONNECT, L"Disconnect");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(hMenu);

        if (cmd == ID_TRAY_OPEN) {
            ShowMainWindow();
        } else if (cmd == ID_TRAY_DISCONNECT) {
            std::thread([this]() { if (controller_) controller_->DisconnectSync(); }).detach();
        } else if (cmd == ID_TRAY_EXIT) {
            ExitApplication();
        }
    }

    void ExitApplication() {
        if (controller_) controller_->DisconnectSync();
        allowClose_ = true;
        if (trayAdded_) {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            trayAdded_ = false;
        }
        DestroyWindow(hwnd_);
    }

    void PaintWindow(HDC hdc) {
        RECT rcClient{};
        GetClientRect(hwnd_, &rcClient);
        int w = rcClient.right - rcClient.left;
        int h = rcClient.bottom - rcClient.top;
        if (w <= 0 || h <= 0) return;

        Bitmap backBuffer(w, h, PixelFormat32bppARGB);
        Graphics g(&backBuffer);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        const Color colBg(255, 0x11, 0x13, 0x15);
        const Color colBorder(255, 0x30, 0x36, 0x39);
        const Color colCard(255, 0x1D, 0x21, 0x24);
        const Color colActiveCard(255, 0x25, 0x2D, 0x2D);
        const Color colHoverCard(255, 0x23, 0x28, 0x2C);
        const Color colInk(255, 0xD2, 0xD6, 0xD8);
        const Color colMuted(255, 0x89, 0x91, 0x96);
        const Color colGreen(255, 0x66, 0xC9, 0x95);
        const Color colOrange(255, 0xE6, 0xAE, 0x60);
        const Color colRed(255, 0xDC, 0x7E, 0x7E);

        g.Clear(colBg);

        SolidBrush brInk(colInk);
        SolidBrush brMuted(colMuted);
        SolidBrush brGreen(colGreen);
        SolidBrush brOrange(colOrange);
        SolidBrush brRed(colRed);
        SolidBrush brCard(colCard);
        SolidBrush brActiveCard(colActiveCard);
        SolidBrush brHoverCard(colHoverCard);

        FontFamily ff(L"Segoe UI");
        StringFormat sfCenter;
        sfCenter.SetAlignment(StringAlignmentCenter);
        sfCenter.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfLeftMid;
        sfLeftMid.SetAlignment(StringAlignmentNear);
        sfLeftMid.SetLineAlignment(StringAlignmentCenter);
        sfLeftMid.SetTrimming(StringTrimmingEllipsisCharacter);
        sfLeftMid.SetFormatFlags(StringFormatFlagsNoWrap);

        StringFormat sfRightMid;
        sfRightMid.SetAlignment(StringAlignmentFar);
        sfRightMid.SetLineAlignment(StringAlignmentCenter);
        sfRightMid.SetTrimming(StringTrimmingEllipsisCharacter);
        sfRightMid.SetFormatFlags(StringFormatFlagsNoWrap);

        // --- TopBar (y: 0..58) ---
        {
            Font fontBrand(&ff, S(27.0f), FontStyleBold, UnitPixel);
            RectF rBrand(S(24.0f), S(2.0f), S(160.0f), S(54.0f));
            g.DrawString(L"echo", -1, &fontBrand, rBrand, &sfLeftMid, &brInk);

            Font fontVer(&ff, S(11.0f), FontStyleRegular, UnitPixel);
            RectF rVer(S(190.0f), S(0.0f), S(141.0f), S(58.0f));
            g.DrawString(L"VPN / 1.1.0", -1, &fontVer, rVer, &sfRightMid, &brMuted);

            auto drawChromeBtn = [&](const RectF& r, HitZone hz, const wchar_t* glyph) {
                bool hov = (hoverZone_ == hz);
                bool prs = (pressZone_ == hz && hov);
                if (hov || prs) {
                    SolidBrush b(prs ? Color(255, 0x30, 0x37, 0x3A) : Color(255, 0x25, 0x2A, 0x2D));
                    FillRoundRect(g, &b, r, S(8.0f));
                }
                Font fBtn(&ff, S(15.0f), FontStyleRegular, UnitPixel);
                g.DrawString(glyph, -1, &fBtn, r, &sfCenter, hov ? &brInk : &brMuted);
            };
            drawChromeBtn(MinimizeRect(), HitZone::MinimizeBtn, L"\x2014");
            drawChromeBtn(CloseRect(), HitZone::CloseBtn, L"\x00D7");
        }

        // --- Body (ActivationView vs MainView) ---
        if (!activated_) {
            Font fTitle(&ff, S(26.0f), FontStyleBold, UnitPixel);
            Font fSub(&ff, S(14.0f), FontStyleRegular, UnitPixel);
            Font fErr(&ff, S(12.0f), FontStyleRegular, UnitPixel);
            Font fBtn(&ff, S(16.0f), FontStyleRegular, UnitPixel);
            Font fFoot(&ff, S(12.0f), FontStyleRegular, UnitPixel);

            g.DrawString(L"Welcome to Echo", -1, &fTitle, RectF(S(28.0f), S(270.0f), S(364.0f), S(40.0f)), &sfCenter, &brInk);
            g.DrawString(L"Please enter your license first.", -1, &fSub, RectF(S(28.0f), S(320.0f), S(364.0f), S(26.0f)), &sfCenter, &brMuted);

            // Input container behind native EDIT control
            FillRoundRect(g, &brCard, InputBoxRect(), S(8.0f));

            if (!licenseError_.empty()) {
                g.DrawString(licenseError_.c_str(), -1, &fErr, RectF(S(28.0f), S(424.0f), S(364.0f), S(38.0f)), &sfCenter, &brRed);
            }

            RectF rAct = ActivateRect();
            bool hov = (hoverZone_ == HitZone::ActivateBtn);
            bool prs = (pressZone_ == HitZone::ActivateBtn && hov);
            Color actCol = prs ? Color(255, 0x40, 0x50, 0x52)
                         : hov ? Color(255, 0x36, 0x40, 0x43)
                               : Color(255, 0x2C, 0x34, 0x36);
            SolidBrush brAct(actCol);
            FillRoundRect(g, &brAct, rAct, S(12.0f));
            g.DrawString(L"Activate", -1, &fBtn, rAct, &sfCenter, &brInk);

            g.DrawString(L"Activate once. Stay ready.", -1, &fFoot, RectF(S(28.0f), S(536.0f), S(364.0f), S(24.0f)), &sfCenter, &brMuted);
        } else {
            // --- Power Button (190x190 circle) ---
            RectF rPow = PowerRect();
            bool connected = (snap_.state == "Connected");
            bool powHov = (hoverZone_ == HitZone::PowerBtn) && (snap_.state != "Disconnecting");
            Color powBg = connected
                ? (powHov ? Color(255, 38, 102, 72) : Color(255, 32, 88, 62))
                : (powHov ? Color(255, 14, 18, 19) : Color(255, 7, 9, 9));
            Color powBorder = connected
                ? Color(255, 102, 201, 149)
                : (powHov ? Color(255, 68, 78, 82) : Color(255, 48, 56, 59));

            SolidBrush brPow(powBg);
            Pen penPow(powBorder, S(1.2f));
            g.FillEllipse(&brPow, rPow);
            g.DrawEllipse(&penPow, rPow);

            // Crisp vector power glyph (arc + top bar)
            float cx = rPow.X + rPow.Width * 0.5f;
            float cy = rPow.Y + rPow.Height * 0.44f;
            float rad = S(24.0f);
            Pen penIcon(colInk, S(3.2f));
            penIcon.SetStartCap(LineCapRound);
            penIcon.SetEndCap(LineCapRound);
            g.DrawArc(&penIcon, cx - rad, cy - rad, rad * 2.0f, rad * 2.0f, -52.0f, 284.0f);
            g.DrawLine(&penIcon, cx, cy - rad - S(4.0f), cx, cy - S(2.0f));

            std::wstring powLabel = L"Connect";
            if (snap_.state == "Connected") powLabel = L"Connected";
            else if (snap_.state == "Checking") powLabel = L"Checking\x2026";
            else if (snap_.state == "Connecting") powLabel = L"Connecting\x2026";
            else if (snap_.state == "Reconnecting") powLabel = L"Reconnecting\x2026";
            else if (snap_.state == "Disconnecting") powLabel = L"Disconnecting\x2026";

            Font fPow(&ff, S(13.0f), FontStyleBold, UnitPixel);
            g.DrawString(powLabel.c_str(), -1, &fPow, RectF(rPow.X, cy + rad + S(10.0f), rPow.Width, S(24.0f)), &sfCenter, &brInk);

            // --- DetailText ---
            std::wstring detail;
            if (!snap_.message.empty()) {
                detail = ToWide(snap_.message);
            } else if (snap_.state == "Connected") {
                detail = ToWide(snap_.activeName + " \xC2\xB7 Automatic\n\xE2\x86\x93 " +
                                VpnConfigFactory::FormatRate(snap_.downloadRate) + "    \xE2\x86\x91 " +
                                VpnConfigFactory::FormatRate(snap_.uploadRate));
            } else if (snap_.state == "Checking") {
                detail = L"Fresh HTTPS checks through each VMess server\nBest verified server connects automatically.";
            } else if (snap_.state == "Connecting" || snap_.state == "Reconnecting") {
                detail = ToWide(snap_.state) + L" \x00B7 Automatic server selection";
            } else if (snap_.completed == 0) {
                detail = L"Tap Connect to test all 3 servers\nand connect to the best one.";
            } else {
                detail = L"Tap Connect to run fresh checks and connect automatically.";
            }
            Font fDetail(&ff, S(12.0f), FontStyleRegular, UnitPixel);
            g.DrawString(detail.c_str(), -1, &fDetail, RectF(S(20.0f), S(254.0f), S(380.0f), S(62.0f)), &sfCenter, &brMuted);

            // --- ScanLabel + VMESS ---
            std::wstring scanLabel;
            if (snap_.state == "Checking") {
                scanLabel = L"Checking " + std::to_wstring(snap_.completed) + L"/" + std::to_wstring(snap_.total) + L" \x00B7 Automatic";
            } else if (snap_.completed == 0) {
                scanLabel = L"3 SERVERS \x00B7 AUTOMATIC";
            } else {
                scanLabel = L"Last check \x00B7 " + std::to_wstring(snap_.completed) + L"/" + std::to_wstring(snap_.total) +
                            L" \x00B7 " + std::to_wstring(snap_.available) + L" available";
            }
            Font fScan(&ff, S(11.0f), FontStyleRegular, UnitPixel);
            Font fVmess(&ff, S(10.0f), FontStyleBold, UnitPixel);
            g.DrawString(scanLabel.c_str(), -1, &fScan, RectF(S(20.0f), S(316.0f), S(310.0f), S(36.0f)), &sfLeftMid, &brMuted);
            g.DrawString(L"VMESS", -1, &fVmess, RectF(S(330.0f), S(316.0f), S(70.0f), S(36.0f)), &sfRightMid, &brGreen);

            // --- Indeterminate ScanProgress bar ---
            if (snap_.state == "Checking") {
                RectF rTrack(S(20.0f), S(352.0f), S(380.0f), S(4.0f));
                SolidBrush brTrack(colBorder);
                FillRoundRect(g, &brTrack, rTrack, S(2.0f));
                float segW = rTrack.Width * 0.32f;
                float travel = rTrack.Width + segW;
                float sx = rTrack.X - segW + progressPhase_ * travel;
                float clipLeft = std::max(rTrack.X, sx);
                float clipRight = std::min(rTrack.X + rTrack.Width, sx + segW);
                if (clipRight > clipLeft) {
                    FillRoundRect(g, &brGreen, RectF(clipLeft, rTrack.Y, clipRight - clipLeft, rTrack.Height), S(2.0f));
                }
            }

            // --- Server Cards ---
            Font fSrvName(&ff, S(15.0f), FontStyleRegular, UnitPixel);
            Font fSrvSub(&ff, S(10.0f), FontStyleRegular, UnitPixel);
            Font fSrvVal(&ff, S(12.0f), FontStyleRegular, UnitPixel);

            for (int i = 0; i < static_cast<int>(snap_.ranked.size()) && i < 3; ++i) {
                const ServerEntry& e = snap_.ranked[i];
                RectF rc = CardRect(i);
                HitZone cz = static_cast<HitZone>(static_cast<int>(HitZone::ServerCard0) + i);
                const Brush* cardBrush = e.isActive ? &brActiveCard : ((hoverZone_ == cz) ? &brHoverCard : &brCard);
                FillRoundRect(g, cardBrush, rc, S(10.0f));

                // Status dot
                float dotCx = rc.X + S(19.0f);
                float dotCy = rc.Y + rc.Height * 0.5f;
                float dotR  = S(5.0f);
                if (e.isActive) {
                    g.FillEllipse(&brGreen, dotCx - dotR, dotCy - dotR, dotR * 2.0f, dotR * 2.0f);
                } else {
                    Pen pDot(colMuted, S(1.5f));
                    g.DrawEllipse(&pDot, dotCx - dotR, dotCy - dotR, dotR * 2.0f, dotR * 2.0f);
                }

                // Name + Subtitle
                std::wstring wname = ToWide(e.profile.name);
                std::wstring wsub  = ToWide(e.Subtitle());
                g.DrawString(wname.c_str(), -1, &fSrvName, RectF(rc.X + S(42.0f), rc.Y + S(12.0f), S(235.0f), S(24.0f)), &sfLeftMid, &brInk);
                g.DrawString(wsub.c_str(),  -1, &fSrvSub,  RectF(rc.X + S(42.0f), rc.Y + S(38.0f), S(235.0f), S(18.0f)), &sfLeftMid, &brMuted);

                // ValueText
                std::wstring wval = ToWide(e.ValueText());
                const Brush* valBrush = &brMuted;
                if (e.state == ProbeState::Failed) valBrush = &brRed;
                else if (e.state == ProbeState::Ok) {
                    if (e.ms < 500.0) valBrush = &brGreen;
                    else if (e.ms <= 1500.0) valBrush = &brOrange;
                    else valBrush = &brRed;
                }
                g.DrawString(wval.c_str(), -1, &fSrvVal, RectF(rc.X + rc.Width - S(105.0f), rc.Y, S(93.0f), rc.Height), &sfRightMid, valBrush);
            }
        }

        // --- Footer Credit ---
        {
            Font fCred(&ff, S(10.0f), FontStyleRegular, UnitPixel);
            g.DrawString(L"developer: Ramtin Ahmadi", -1, &fCred, RectF(S(20.0f), S(806.0f), S(380.0f), S(34.0f)), &sfRightMid, &brMuted);
        }

        // --- Outer 1px border ---
        {
            Pen penBorder(colBorder, 1.0f);
            g.DrawRectangle(&penBorder, 0.5f, 0.5f, static_cast<REAL>(w) - 1.0f, static_cast<REAL>(h) - 1.0f);
        }

        Graphics screenG(hdc);
        screenG.DrawImage(&backBuffer, 0, 0);
    }

    static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<MainWindowApp*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        if (msg == WM_KEYDOWN && wParam == VK_RETURN && self) {
            self->TryActivate();
            return 0;
        }
        if (msg == WM_CHAR && wParam == '\r') {
            return 0;
        }
        return CallWindowProcW(self->origEditProc_, hWnd, msg, wParam, lParam);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE: {
                HDC hdc = GetDC(hwnd_);
                dpiScale_ = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSX)) / 96.0f;
                if (dpiScale_ < 1.0f) dpiScale_ = 1.0f;
                ReleaseDC(hwnd_, hdc);

                editBgBrush_ = CreateSolidBrush(RGB(0x1D, 0x21, 0x24));
                hEdit_ = CreateWindowExW(
                    0, L"EDIT", L"",
                    WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE,
                    SI(44.0f), SI(385.0f), SI(332.0f), SI(24.0f),
                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LICENSE_EDIT)), hInst_, nullptr
                );
                SetWindowLongPtrW(hEdit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                origEditProc_ = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(hEdit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&EditSubclassProc)));

                activated_ = LicenseStore::IsActivated();
                UpdateEditFontAndBounds();
                if (!activated_) SetFocus(hEdit_);

                // Setup Tray Icon
                nid_.cbSize = sizeof(nid_);
                nid_.hWnd = hwnd_;
                nid_.uID = 1;
                nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                nid_.uCallbackMessage = WM_APP_TRAYICON;
                nid_.hIcon = hSmallIcon_ ? hSmallIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
                std::wcsncpy(nid_.szTip, L"VPN Echo \x00B7 Disconnected", 127);
                trayAdded_ = (Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE);

                SetTimer(hwnd_, TIMER_RENDER,   400,  nullptr);
                SetTimer(hwnd_, TIMER_TRAFFIC,  1000, nullptr);
                SetTimer(hwnd_, TIMER_PROGRESS, 33,   nullptr);
                return 0;
            }

            case WM_DPICHANGED: {
                UINT newDpi = HIWORD(wParam);
                dpiScale_ = static_cast<float>(newDpi) / 96.0f;
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             SI(420.0f), SI(840.0f), SWP_NOZORDER | SWP_NOACTIVATE);
                UpdateEditFontAndBounds();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            case WM_CTLCOLOREDIT: {
                HDC hdcEdit = reinterpret_cast<HDC>(wParam);
                SetTextColor(hdcEdit, RGB(0xD2, 0xD6, 0xD8));
                SetBkColor(hdcEdit, RGB(0x1D, 0x21, 0x24));
                return reinterpret_cast<LRESULT>(editBgBrush_);
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd_, &ps);
                PaintWindow(hdc);
                EndPaint(hwnd_, &ps);
                return 0;
            }

            case WM_SETCURSOR: {
                if (LOWORD(lParam) == HTCLIENT && hoverZone_ != HitZone::None && hoverZone_ != HitZone::CreditArea) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                break;
            }

            case WM_MOUSEMOVE: {
                float mx = static_cast<float>(GET_X_LPARAM(lParam));
                float my = static_cast<float>(GET_Y_LPARAM(lParam));
                HitZone nz = HitTestControls(mx, my);
                if (nz != hoverZone_) {
                    hoverZone_ = nz;
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                    TrackMouseEvent(&tme);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }

            case WM_MOUSELEAVE: {
                if (hoverZone_ != HitZone::None) {
                    hoverZone_ = HitZone::None;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONDOWN: {
                float mx = static_cast<float>(GET_X_LPARAM(lParam));
                float my = static_cast<float>(GET_Y_LPARAM(lParam));
                HitZone hz = HitTestControls(mx, my);
                if (hz == HitZone::None && my < S(58.0f)) {
                    ReleaseCapture();
                    SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    return 0;
                }
                pressZone_ = hz;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            case WM_LBUTTONUP: {
                float mx = static_cast<float>(GET_X_LPARAM(lParam));
                float my = static_cast<float>(GET_Y_LPARAM(lParam));
                HitZone hz = HitTestControls(mx, my);
                HitZone wasPressed = pressZone_;
                pressZone_ = HitZone::None;
                if (GetCapture() == hwnd_) ReleaseCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);

                if (hz != HitZone::None && hz == wasPressed) {
                    switch (hz) {
                        case HitZone::MinimizeBtn:
                            ShowWindow(hwnd_, SW_MINIMIZE);
                            break;
                        case HitZone::CloseBtn:
                            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                            break;
                        case HitZone::ActivateBtn:
                            TryActivate();
                            break;
                        case HitZone::PowerBtn:
                            if (controller_ && snap_.state != "Disconnecting") controller_->ToggleAsync();
                            break;
                        case HitZone::ServerCard0:
                        case HitZone::ServerCard1:
                        case HitZone::ServerCard2:
                            ShowServerDetails(static_cast<int>(hz) - static_cast<int>(HitZone::ServerCard0));
                            break;
                        default:
                            break;
                    }
                }
                return 0;
            }

            case WM_RBUTTONUP: {
                float mx = static_cast<float>(GET_X_LPARAM(lParam));
                float my = static_cast<float>(GET_Y_LPARAM(lParam));
                if (CreditRect().Contains(mx, my)) {
                    ShowAboutDialog();
                }
                return 0;
            }

            case WM_TIMER: {
                if (wParam == TIMER_RENDER) {
                    RefreshSnapshotAndTray();
                } else if (wParam == TIMER_TRAFFIC) {
                    if (controller_) controller_->TickFromTimer();
                } else if (wParam == TIMER_PROGRESS) {
                    if (snap_.state == "Checking" && IsWindowVisible(hwnd_)) {
                        progressPhase_ += 0.022f;
                        if (progressPhase_ > 1.0f) progressPhase_ -= 1.0f;
                        RECT rBar{SI(18.0f), SI(350.0f), SI(402.0f), SI(358.0f)};
                        InvalidateRect(hwnd_, &rBar, FALSE);
                    }
                }
                return 0;
            }

            case WM_APP_CHANGED:
                RefreshSnapshotAndTray();
                return 0;

            case WM_APP_TRAYICON: {
                if (lParam == WM_LBUTTONDBLCLK) {
                    ShowMainWindow();
                } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                    ShowTrayMenu();
                }
                return 0;
            }

            case WM_CLOSE: {
                if (!allowClose_ && snap_.state != "Disconnected") {
                    ShowWindow(hwnd_, SW_HIDE);
                    return 0;
                }
                if (trayAdded_) {
                    Shell_NotifyIconW(NIM_DELETE, &nid_);
                    trayAdded_ = false;
                }
                DestroyWindow(hwnd_);
                return 0;
            }

            case WM_DESTROY: {
                KillTimer(hwnd_, TIMER_RENDER);
                KillTimer(hwnd_, TIMER_TRAFFIC);
                KillTimer(hwnd_, TIMER_PROGRESS);
                if (trayAdded_) {
                    Shell_NotifyIconW(NIM_DELETE, &nid_);
                    trayAdded_ = false;
                }
                if (editFont_) { DeleteObject(editFont_); editFont_ = nullptr; }
                if (editBgBrush_) { DeleteObject(editBgBrush_); editBgBrush_ = nullptr; }
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MainWindowApp* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindowApp*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<MainWindowApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self) return self->HandleMessage(msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

public:
    int Run(HINSTANCE hInstance, int nCmdShow) {
        hInst_ = hInstance;

        try {
            controller_ = std::make_unique<VpnController>([this]() {
                if (hwnd_) PostMessageW(hwnd_, WM_APP_CHANGED, 0, 0);
            });
            snap_ = controller_->GetSnapshot();
        } catch (const std::exception& ex) {
            std::wstring err = L"VPN Echo could not load its three bundled VMess profiles. Repair or reinstall the app.\n\n" + ToWide(ex.what());
            MessageBoxW(nullptr, err.c_str(), L"VPN Echo", MB_OK | MB_ICONERROR);
            return 1;
        }

        hAppIcon_ = static_cast<HICON>(LoadImageW(hInst_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
        hSmallIcon_ = static_cast<HICON>(LoadImageW(hInst_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        if (!hSmallIcon_ && FileExistsW(DataPaths::IconPath())) {
            hSmallIcon_ = static_cast<HICON>(LoadImageW(nullptr, DataPaths::IconPath().c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &StaticWndProc;
        wc.hInstance = hInst_;
        wc.hIcon = hAppIcon_ ? hAppIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hSmallIcon_ ? hSmallIcon_ : wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"VpnEchoMainWindowClass";
        RegisterClassExW(&wc);

        HDC screenDc = GetDC(nullptr);
        float initScale = static_cast<float>(GetDeviceCaps(screenDc, LOGPIXELSX)) / 96.0f;
        if (initScale < 1.0f) initScale = 1.0f;
        int winW = static_cast<int>(std::lround(420.0f * initScale));
        int winH = static_cast<int>(std::lround(840.0f * initScale));
        int scrW = GetSystemMetrics(SM_CXSCREEN);
        int scrH = GetSystemMetrics(SM_CYSCREEN);
        ReleaseDC(nullptr, screenDc);

        int posX = std::max(0, (scrW - winW) / 2);
        int posY = std::max(0, (scrH - winH) / 2);

        hwnd_ = CreateWindowExW(
            WS_EX_APPWINDOW,
            wc.lpszClassName,
            L"VPN Echo",
            WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
            posX, posY, winW, winH,
            nullptr, nullptr, hInst_, this
        );
        if (!hwnd_) return 1;

        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        controller_.reset();
        return static_cast<int>(msg.wParam);
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\VPN-Echo-Windows-1.1");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"VPN Echo is already running. Check the notification area.", L"VPN Echo", MB_OK | MB_ICONINFORMATION);
        CloseHandle(hMutex);
        return 0;
    }

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    int exitCode = 0;
    {
        MainWindowApp app;
        exitCode = app.Run(hInstance, nCmdShow);
    }

    GdiplusShutdown(gdiplusToken);
    WSACleanup();
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return exitCode;
}
