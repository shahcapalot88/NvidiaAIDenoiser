// OptiX AI Denoiser GUI - native Win32 front-end for the Denoiser.exe command line tool.
// Author: Subhajit Maji. Copyright (c) 2026 Subhajit Maji. All rights reserved.
//
// Design note: Denoiser.exe keeps global state and calls exit() on any error, so each image
// is processed by launching it as a child process and streaming its output into the log.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "version.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

// ------------------------------------------------------------------ palette (NVIDIA App look)
static const COLORREF cBg     = RGB(18, 18, 18);
static const COLORREF cHeader = RGB(11, 11, 11);
static const COLORREF cPanel  = RGB(30, 30, 30);
static const COLORREF cPanel2 = RGB(42, 42, 42);
static const COLORREF cField  = RGB(23, 23, 23);
static const COLORREF cLine   = RGB(56, 56, 56);
static const COLORREF cFg     = RGB(237, 237, 237);
static const COLORREF cDim    = RGB(154, 154, 154);
static const COLORREF cFoot   = RGB(110, 110, 110);
static const COLORREF cAccent = RGB(118, 185, 0);   // NVIDIA green #76B900
static const COLORREF cRed    = RGB(232, 96, 96);

// ------------------------------------------------------------------ ids / messages
enum {
    ID_ADD = 101, ID_REMOVE, ID_CLEAR, ID_LIST,
    ID_OUTDIR, ID_OUTBROWSE, ID_SUFFIX, ID_BLEND, ID_HDR, ID_GPU, ID_DETECT,
    ID_ALBEDO, ID_ALBEDOB, ID_NORMAL, ID_NORMALB, ID_EXE, ID_EXEB,
    ID_LOG, ID_CANCEL, ID_START
};
enum {
    WM_APP_LOG = WM_APP + 1,   // lParam = new std::wstring*
    WM_APP_STATUS,             // wParam = item index, lParam = new std::wstring*
    WM_APP_TEXT,               // lParam = new std::wstring*
    WM_APP_PROGRESS,           // wParam = percent
    WM_APP_DONE                // wParam = ok count, lParam = failed count
};

// ------------------------------------------------------------------ state
struct Item  { std::wstring path; std::wstring status = L"Queued"; };
struct Label { RECT r; std::wstring t; int kind; UINT fmt; };   // kind: 0 dim, 1 section, 2 hint

struct App {
    HINSTANCE hinst = nullptr;
    HWND hwnd = nullptr;
    int dpi = 96;
    bool ready = false;

    HFONT fBody = nullptr, fBold = nullptr, fTitle = nullptr, fSmall = nullptr,
          fSect = nullptr, fTiny = nullptr, fMono = nullptr;
    HBRUSH brField = nullptr;

    HWND list = nullptr, bAdd = nullptr, bRemove = nullptr, bClear = nullptr;
    HWND eOutDir = nullptr, bOutBrowse = nullptr, eSuffix = nullptr, sBlend = nullptr, bHdr = nullptr;
    HWND eGpu = nullptr, bDetect = nullptr, eAlbedo = nullptr, bAlbedoB = nullptr;
    HWND eNormal = nullptr, bNormalB = nullptr, eExe = nullptr, bExeB = nullptr;
    HWND eLog = nullptr, bCancel = nullptr, bStart = nullptr;
    HWND hover = nullptr;

    RECT rHeader{}, rFooter{}, rAction{}, rQueue{}, rSettings{}, rLog{}, rList{}, rBlendVal{}, rStatus{}, rProg{};
    std::vector<Label> labels;

    std::vector<Item> items;
    double blend = 0.0;
    bool hdr = true;

    bool busy = false, indet = false;
    int progress = 0, anim = 0;
    std::wstring statusText = L"Ready";

    std::atomic<bool> cancel{false};
    std::thread worker;
    std::mutex procMu;
    HANDLE hProc = nullptr;
    HANDLE job = nullptr;
    std::wstring ini;
};
static App g;

static int S(int v) { return MulDiv(v, g.dpi, 96); }

// ------------------------------------------------------------------ small helpers
static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n > 0) GetWindowTextW(h, &s[0], n + 1);
    return s;
}
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static std::wstring FileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
static std::wstring ParentDir(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}
static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring ExeDir() {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, b, (DWORD)(sizeof(b) / sizeof(b[0])));
    return ParentDir(std::wstring(b, n));
}
static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    UINT cp = CP_UTF8; DWORD fl = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, fl, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) { cp = CP_ACP; fl = 0; n = MultiByteToWideChar(cp, fl, s.data(), (int)s.size(), nullptr, 0); }
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, fl, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static bool HasImageExt(const std::wstring& p) {
    size_t d = p.find_last_of(L'.');
    if (d == std::wstring::npos) return false;
    std::wstring e = p.substr(d);
    for (auto& ch : e) ch = (wchar_t)towlower(ch);
    static const wchar_t* ex[] = { L".png", L".jpg", L".jpeg", L".tif", L".tiff", L".exr", L".bmp", L".tga", L".hdr" };
    for (auto x : ex) if (e == x) return true;
    return false;
}
static bool SamePath(const std::wstring& a, const std::wstring& b) {
    wchar_t x[MAX_PATH * 2], y[MAX_PATH * 2];
    DWORD nx = GetFullPathNameW(a.c_str(), MAX_PATH * 2, x, nullptr);
    DWORD ny = GetFullPathNameW(b.c_str(), MAX_PATH * 2, y, nullptr);
    if (!nx || !ny) return _wcsicmp(a.c_str(), b.c_str()) == 0;
    return _wcsicmp(x, y) == 0;
}
static std::wstring BuildOut(const std::wstring& in, const std::wstring& outDir, const std::wstring& suffix) {
    std::wstring dir = outDir.empty() ? ParentDir(in) : outDir;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    std::wstring name = FileName(in), stem = name, ext;
    size_t d = name.find_last_of(L'.');
    if (d != std::wstring::npos) { stem = name.substr(0, d); ext = name.substr(d); }
    return (dir.empty() ? L"" : dir + L"\\") + stem + suffix + ext;
}
// Command line quoting that matches the standard CRT parsing rules.
static std::wstring Quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
        if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r.push_back(L'"'); }
        else { r.append(bs, L'\\'); r.push_back(a[i]); }
    }
    r.push_back(L'"');
    return r;
}

// ------------------------------------------------------------------ GDI helpers
static HFONT MkFont(int px, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}
static void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}
static void RoundBox(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border) {
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HBRUSH b = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, b);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(rad) * 2, S(rad) * 2);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(p); DeleteObject(b);
}
static void Text(HDC dc, HFONT f, COLORREF c, const std::wstring& s, RECT r, UINT fmt) {
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
}
struct Run { std::wstring t; HFONT f; COLORREF c; };
static int DrawRuns(HDC dc, int x, int y, int h, const std::vector<Run>& runs) {
    for (auto& r : runs) {
        SelectObject(dc, r.f);
        SIZE sz{}; GetTextExtentPoint32W(dc, r.t.c_str(), (int)r.t.size(), &sz);
        RECT rc{ x, y, x + sz.cx + 2, y + h };
        Text(dc, r.f, r.c, r.t, rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += sz.cx;
    }
    return x;
}

static void MakeFonts() {
    HFONT* all[] = { &g.fBody, &g.fBold, &g.fTitle, &g.fSmall, &g.fSect, &g.fTiny, &g.fMono };
    for (auto f : all) if (*f) { DeleteObject(*f); *f = nullptr; }
    g.fBody = MkFont(13, FW_NORMAL);
    g.fBold = MkFont(13, FW_SEMIBOLD);
    g.fTitle = MkFont(20, FW_BOLD);
    g.fSmall = MkFont(12, FW_NORMAL);
    g.fSect = MkFont(12, FW_BOLD);
    g.fTiny = MkFont(11, FW_NORMAL);
    g.fMono = MkFont(12, FW_NORMAL, L"Consolas");
}
static void ApplyFonts() {
    HWND body[] = { g.eOutDir, g.eSuffix, g.eGpu, g.eAlbedo, g.eNormal, g.eExe };
    for (HWND h : body) {
        SendMessageW(h, WM_SETFONT, (WPARAM)g.fBody, TRUE);
        SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    }
    SendMessageW(g.eLog, WM_SETFONT, (WPARAM)g.fMono, TRUE);
}

// ------------------------------------------------------------------ layout
static void Layout() {
    if (!g.ready) return;
    RECT rc; GetClientRect(g.hwnd, &rc);
    int W = rc.right, H = rc.bottom, M = S(24), pad = S(16);

    g.rHeader = { 0, 0, W, S(64) };
    int footH = S(64), actH = S(64);
    g.rFooter = { 0, H - footH, W, H };
    g.rAction = { 0, H - footH - actH, W, H - footH };
    int top = g.rHeader.bottom + S(16), bot = g.rAction.top - S(4);
    int setW = S(360);
    g.rSettings = { W - M - setW, top, W - M, bot };
    int leftR = g.rSettings.left - S(16);
    g.rLog = { M, bot - S(150), leftR, bot };
    g.rQueue = { M, top, leftR, g.rLog.top - S(12) };

    g.labels.clear();
    auto Lbl = [&](int x, int y, int w, int h, const wchar_t* t, int kind, UINT fmt = DT_LEFT) {
        g.labels.push_back(Label{ { x, y, x + w, y + h }, t, kind, fmt });
    };
    auto place = [&](HWND h, int x, int y, int w, int hh) { MoveWindow(h, x, y, w, hh, TRUE); };
    // An edit sits inside a 30px tall field; the field itself is painted by the parent.
    auto fieldRow = [&](int x, int y, int w, HWND edit, HWND browse) {
        int fw = browse ? w - S(48) : w;
        place(edit, x + S(8), y + S(6), fw - S(16), S(18));
        if (browse) place(browse, x + w - S(40), y, S(40), S(30));
    };

    // --- queue panel
    int qx = g.rQueue.left + pad, qy = g.rQueue.top + pad, qw = g.rQueue.right - g.rQueue.left - 2 * pad;
    int tby = qy + S(26);
    place(g.bAdd, qx, tby, S(124), S(32));
    place(g.bRemove, qx + S(132), tby, S(84), S(32));
    place(g.bClear, qx + S(224), tby, S(70), S(32));
    int engLabelY = g.rQueue.bottom - pad - S(30) - S(20);
    g.rList = { qx, tby + S(32) + S(12), qx + qw, engLabelY - S(12) };
    place(g.list, g.rList.left + S(4), g.rList.top + S(4), g.rList.right - g.rList.left - S(8),
          g.rList.bottom - g.rList.top - S(8));
    Lbl(qx, engLabelY, qw, S(16), L"Engine (Denoiser.exe)", 0);
    fieldRow(qx, engLabelY + S(20), qw, g.eExe, g.bExeB);

    // --- settings panel
    int sx = g.rSettings.left + pad, sw = g.rSettings.right - g.rSettings.left - 2 * pad, c = g.rSettings.top + pad;
    Lbl(sx, c, sw, S(16), L"OUTPUT", 1); c += S(24);
    Lbl(sx, c, sw, S(16), L"Output folder (blank = same as input)", 0); c += S(18);
    fieldRow(sx, c, sw, g.eOutDir, g.bOutBrowse); c += S(30) + S(12);
    Lbl(sx, c, sw, S(16), L"Filename suffix", 0); c += S(18);
    fieldRow(sx, c, sw, g.eSuffix, nullptr); c += S(30) + S(18);

    Lbl(sx, c, sw, S(16), L"DENOISER", 1); c += S(24);
    Lbl(sx, c, sw - S(50), S(16), L"Blend (0 = fully denoised, 1 = original)", 0);
    g.rBlendVal = { sx + sw - S(50), c, sx + sw, c + S(16) }; c += S(20);
    place(g.sBlend, sx, c, sw, S(24)); c += S(24) + S(10);
    place(g.bHdr, sx, c, sw, S(24)); c += S(24) + S(12);
    Lbl(sx, c, sw, S(16), L"GPU index", 0); c += S(18);
    fieldRow(sx, c, S(70), g.eGpu, nullptr);
    place(g.bDetect, sx + S(78), c, S(120), S(30)); c += S(30) + S(18);

    Lbl(sx, c, sw, S(16), L"GUIDE LAYERS (optional)", 1);
    Lbl(sx, c, sw, S(16), L"single image only", 2, DT_RIGHT); c += S(24);
    Lbl(sx, c, sw, S(16), L"Albedo", 0); c += S(18);
    fieldRow(sx, c, sw, g.eAlbedo, g.bAlbedoB); c += S(30) + S(12);
    Lbl(sx, c, sw, S(16), L"Normal (requires albedo)", 0); c += S(18);
    fieldRow(sx, c, sw, g.eNormal, g.bNormalB);

    // --- log panel
    int lx = g.rLog.left + pad;
    Lbl(lx, g.rLog.top + S(12), S(200), S(16), L"LOG", 1);
    RECT lf{ lx, g.rLog.top + S(34), g.rLog.right - pad, g.rLog.bottom - S(14) };
    place(g.eLog, lf.left + S(8), lf.top + S(6), lf.right - lf.left - S(16), lf.bottom - lf.top - S(12));

    // --- action bar
    int bx = W - M - S(150), by = g.rAction.top + S(12);
    place(g.bStart, bx, by, S(150), S(40));
    int cx = bx - S(8) - S(96);
    place(g.bCancel, cx, by, S(96), S(40));
    g.rStatus = { M, g.rAction.top + S(10), cx - S(20), g.rAction.top + S(28) };
    g.rProg = { M, g.rAction.top + S(34), cx - S(20), g.rAction.top + S(34) + S(6) };

    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------ painting
static void PaintAll(HDC dc, const RECT& rc) {
    Fill(dc, rc, cBg);

    // header
    Fill(dc, g.rHeader, cHeader);
    Fill(dc, RECT{ 0, g.rHeader.bottom - 1, rc.right, g.rHeader.bottom }, cLine);
    RoundBox(dc, RECT{ S(24), S(14), S(29), S(50) }, 2, cAccent, cAccent);
    Text(dc, g.fTitle, cFg, L"OPTIX AI DENOISER", RECT{ S(42), S(11), rc.right, S(38) }, DT_LEFT | DT_SINGLELINE);
    Text(dc, g.fSmall, cDim, L"GPU-accelerated noise and grain removal", RECT{ S(42), S(38), rc.right, S(56) },
         DT_LEFT | DT_SINGLELINE);

    // panels
    RoundBox(dc, g.rQueue, 6, cPanel, cLine);
    RoundBox(dc, g.rSettings, 6, cPanel, cLine);
    RoundBox(dc, g.rLog, 6, cPanel, cLine);

    // queue title + list field
    std::wstring qt = g.items.empty() ? L"INPUT IMAGES" : L"INPUT IMAGES (" + std::to_wstring(g.items.size()) + L")";
    Text(dc, g.fSect, cAccent, qt, RECT{ g.rQueue.left + S(16), g.rQueue.top + S(16), g.rQueue.right, g.rQueue.top + S(32) },
         DT_LEFT | DT_SINGLELINE);
    RoundBox(dc, g.rList, 4, cField, cLine);
    if (g.items.empty())
        Text(dc, g.fBody, cDim, L"Drag and drop images here", g.rList, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // labels
    for (auto& lb : g.labels) {
        switch (lb.kind) {
        case 1: Text(dc, g.fSect, cAccent, lb.t, lb.r, lb.fmt | DT_SINGLELINE); break;
        case 2: Text(dc, g.fTiny, cDim, lb.t, lb.r, lb.fmt | DT_SINGLELINE | DT_VCENTER); break;
        default: Text(dc, g.fSmall, cDim, lb.t, lb.r, lb.fmt | DT_SINGLELINE); break;
        }
    }
    wchar_t bv[16]; swprintf_s(bv, L"%.2f", g.blend);
    Text(dc, g.fBold, cAccent, bv, g.rBlendVal, DT_RIGHT | DT_SINGLELINE);

    // field backgrounds behind the edit controls
    HWND fe[] = { g.eOutDir, g.eSuffix, g.eGpu, g.eAlbedo, g.eNormal, g.eExe, g.eLog };
    for (HWND h : fe) {
        RECT r; GetWindowRect(h, &r);
        MapWindowPoints(nullptr, g.hwnd, (POINT*)&r, 2);
        InflateRect(&r, S(8), S(6));
        RoundBox(dc, r, 4, cField, GetFocus() == h ? cAccent : cLine);
    }

    // status + progress
    Text(dc, g.fSmall, g.busy ? cFg : cDim, g.statusText, g.rStatus, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RoundBox(dc, g.rProg, 3, cPanel2, cPanel2);
    int tw = g.rProg.right - g.rProg.left;
    if (g.busy && g.indet && tw > 8) {
        int seg = tw / 4, pos = g.anim % (tw + seg);
        RECT s{ g.rProg.left + pos - seg, g.rProg.top, g.rProg.left + pos, g.rProg.bottom };
        s.left = std::max(s.left, g.rProg.left); s.right = std::min(s.right, g.rProg.right);
        if (s.right - s.left > 4) RoundBox(dc, s, 3, cAccent, cAccent);
    } else if (g.progress > 0 && tw > 8) {
        RECT f = g.rProg; f.right = f.left + tw * g.progress / 100;
        if (f.right - f.left > 4) RoundBox(dc, f, 3, cAccent, cAccent);
    }

    // footer
    Fill(dc, g.rFooter, cHeader);
    Fill(dc, RECT{ 0, g.rFooter.top, rc.right, g.rFooter.top + 1 }, cLine);
    int M = S(24), fy = g.rFooter.top + S(8);
    DrawRuns(dc, M, fy, S(18), {
        { L"Author: ", g.fSmall, cDim },
        { L"Subhajit Maji", g.fBold, cFg },
        { L"    |    ", g.fSmall, cLine },
        { L"\u00A9 2026 Subhajit Maji. All rights reserved.", g.fSmall, cDim } });
    std::wstring ver = L"Version " WIDEN(APP_VER_STR);
    Text(dc, g.fSmall, cAccent, ver, RECT{ M, fy, rc.right - M, fy + S(18) }, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    Text(dc, g.fTiny, cFoot,
         L"NVIDIA and OptiX are trademarks of NVIDIA Corporation. This app is not affiliated with or endorsed by NVIDIA. "
         L"Denoiser command line core by Declan Russell (MIT License).",
         RECT{ M, fy + S(22), rc.right - M, g.rFooter.bottom - S(4) }, DT_LEFT | DT_WORDBREAK);
}

static void DrawButton(const DRAWITEMSTRUCT* d) {
    HWND h = d->hwndItem; HDC dc = d->hDC; RECT r = d->rcItem; int id = (int)d->CtlID;
    bool dis = (d->itemState & ODS_DISABLED) != 0, down = (d->itemState & ODS_SELECTED) != 0, hot = (g.hover == h);
    Fill(dc, r, (id == ID_START || id == ID_CANCEL) ? cBg : cPanel);

    if (id == ID_HDR) {   // check box
        int bs = S(18);
        RECT box{ r.left, (r.top + r.bottom - bs) / 2, r.left + bs, (r.top + r.bottom - bs) / 2 + bs };
        bool on = g.hdr;
        RoundBox(dc, box, 3, on ? (dis ? cDim : cAccent) : cField, on ? (dis ? cDim : cAccent) : cLine);
        if (on) {
            HPEN p = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(0, 0, 0));
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, box.left + S(4), box.top + S(9), nullptr);
            LineTo(dc, box.left + S(8), box.top + S(13));
            LineTo(dc, box.left + S(14), box.top + S(5));
            SelectObject(dc, op); DeleteObject(p);
        }
        Text(dc, g.fBody, dis ? cDim : cFg, L"Use HDR training data",
             RECT{ box.right + S(10), r.top, r.right, r.bottom }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    bool primary = (id == ID_START);
    COLORREF fill = primary ? cAccent : cPanel2;
    COLORREF border = primary ? cAccent : (hot ? cAccent : cLine);
    COLORREF fg = primary ? RGB(0, 0, 0) : cFg;
    if (down) fill = primary ? RGB(98, 154, 0) : RGB(36, 36, 36);
    if (dis) { fill = primary ? RGB(52, 72, 12) : cPanel; border = cLine; fg = primary ? RGB(120, 120, 120) : cDim; }
    RoundBox(dc, r, 3, fill, border);
    wchar_t t[64]; GetWindowTextW(h, t, 64);
    Text(dc, g.fBold, fg, t, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawListItem(const DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC; RECT r = d->rcItem;
    if ((int)d->itemID < 0 || d->itemID >= g.items.size()) { Fill(dc, r, cField); return; }
    bool sel = (d->itemState & ODS_SELECTED) != 0;
    Fill(dc, r, sel ? cPanel2 : cField);
    if (sel) Fill(dc, RECT{ r.left, r.top, r.left + S(3), r.bottom }, cAccent);

    const Item& it = g.items[d->itemID];
    COLORREF sc = cDim;
    if (it.status == L"Done") sc = cAccent;
    else if (it.status == L"Failed") sc = cRed;
    else if (it.status.rfind(L"Processing", 0) == 0) sc = cFg;

    SelectObject(dc, g.fSmall);
    SIZE sz{}; GetTextExtentPoint32W(dc, it.status.c_str(), (int)it.status.size(), &sz);
    Text(dc, g.fSmall, sc, it.status, RECT{ r.left, r.top, r.right - S(12), r.bottom }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    Text(dc, g.fBody, cFg, FileName(it.path), RECT{ r.left + S(14), r.top, r.right - S(24) - sz.cx, r.bottom },
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ------------------------------------------------------------------ custom child controls
static LRESULT CALLBACK SliderProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        bool en = IsWindowEnabled(h) != FALSE;
        Fill(mem, rc, cPanel);
        int th = S(14), x0 = th / 2, x1 = rc.right - th / 2, cy = rc.bottom / 2;
        int tx = x0 + (int)((x1 - x0) * g.blend);
        RoundBox(mem, RECT{ x0, cy - S(2), x1, cy + S(2) }, 2, cLine, cLine);
        if (tx - x0 > 4) RoundBox(mem, RECT{ x0, cy - S(2), tx, cy + S(2) }, 2, en ? cAccent : cDim, en ? cAccent : cDim);
        HBRUSH b = CreateSolidBrush(en ? RGB(255, 255, 255) : RGB(150, 150, 150));
        HGDIOBJ ob = SelectObject(mem, b), op = SelectObject(mem, GetStockObject(NULL_PEN));
        Ellipse(mem, tx - th / 2, cy - th / 2, tx + th / 2 + 1, cy + th / 2 + 1);
        SelectObject(mem, ob); SelectObject(mem, op); DeleteObject(b);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(h);
        [[fallthrough]];
    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            RECT rc; GetClientRect(h, &rc);
            int th = S(14);
            double v = ((double)GET_X_LPARAM(l) - th / 2.0) / std::max(1, (int)(rc.right - th));
            v = std::min(1.0, std::max(0.0, v));
            g.blend = std::floor(v * 100.0 + 0.5) / 100.0;
            InvalidateRect(h, nullptr, FALSE);
            InvalidateRect(g.hwnd, &g.rBlendVal, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_MOUSEMOVE:
        if (g.hover != h) {
            g.hover = h;
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (g.hover == h) { g.hover = nullptr; InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, BtnSub, 1);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK ListSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_DROPFILES) { SendMessageW(GetParent(h), WM_DROPFILES, w, l); return 0; }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, ListSub, 1);
    return DefSubclassProc(h, m, w, l);
}

// ------------------------------------------------------------------ settings
static std::wstring IniGet(const wchar_t* key, const wchar_t* def) {
    wchar_t b[2048];
    GetPrivateProfileStringW(L"Settings", key, def, b, 2048, g.ini.c_str());
    return b;
}
static void LoadSettings() {
    wchar_t ad[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, ad);
    g.ini = std::wstring(ad) + L"\\OptiXDenoiserGUI\\settings.ini";

    SetWindowTextW(g.eExe, IniGet(L"ExePath", L"").c_str());
    SetWindowTextW(g.eOutDir, IniGet(L"OutputDir", L"").c_str());
    SetWindowTextW(g.eSuffix, IniGet(L"Suffix", L"_denoised").c_str());
    SetWindowTextW(g.eGpu, IniGet(L"Gpu", L"0").c_str());
    g.blend = std::min(1.0, std::max(0.0, _wtof(IniGet(L"Blend", L"0").c_str())));
    g.hdr = IniGet(L"Hdr", L"1") != L"0";
}
static void SaveSettings() {
    if (g.ini.empty()) return;
    SHCreateDirectoryExW(nullptr, ParentDir(g.ini).c_str(), nullptr);
    auto put = [](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"Settings", k, v.c_str(), g.ini.c_str()); };
    wchar_t bl[16]; swprintf_s(bl, L"%.2f", g.blend);
    put(L"ExePath", Trim(GetText(g.eExe)));
    put(L"OutputDir", Trim(GetText(g.eOutDir)));
    put(L"Suffix", GetText(g.eSuffix));
    put(L"Gpu", Trim(GetText(g.eGpu)));
    put(L"Blend", bl);
    put(L"Hdr", g.hdr ? L"1" : L"0");
}
static int ParseGpu() {
    int v = _wtoi(Trim(GetText(g.eGpu)).c_str());
    return v < 0 ? 0 : v;
}
static std::wstring ResolveExe() {
    std::wstring p = Trim(GetText(g.eExe));
    if (!p.empty() && FileExists(p)) return p;
    std::wstring base = ExeDir();
    for (const std::wstring& c : { base + L"\\Denoiser.exe", base + L"\\Denoiser\\Denoiser.exe" })
        if (FileExists(c)) { SetWindowTextW(g.eExe, c.c_str()); return c; }
    return L"";
}

// ------------------------------------------------------------------ process running
static void Post(UINT msg, WPARAM w, const std::wstring& s) {
    PostMessageW(g.hwnd, msg, w, (LPARAM) new std::wstring(s));
}

static DWORD RunProcess(const std::wstring& exe, const std::vector<std::wstring>& args,
                        const std::function<void(const std::wstring&)>& onLine, bool& sawErr, bool track) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { onLine(L"Could not create output pipe."); sawErr = true; return (DWORD)-1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring cl = Quote(exe);
    for (auto& a : args) { cl += L' '; cl += Quote(a); }
    std::vector<wchar_t> buf(cl.begin(), cl.end()); buf.push_back(0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring dir = ParentDir(exe);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        onLine(L"Could not start Denoiser.exe (Windows error " + std::to_wstring(GetLastError()) + L").");
        CloseHandle(rd); sawErr = true; return (DWORD)-1;
    }
    if (g.job) AssignProcessToJobObject(g.job, pi.hProcess);   // child dies if the GUI dies
    if (track) { std::lock_guard<std::mutex> lk(g.procMu); g.hProc = pi.hProcess; }
    ResumeThread(pi.hThread);
    if (track && g.cancel) TerminateProcess(pi.hProcess, 1);

    auto emit = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::wstring w = Widen(line);
        if (w.find(L" ERROR   |") != std::wstring::npos) sawErr = true;
        onLine(w);
    };
    std::string acc; char tmp[4096]; DWORD n = 0;
    while (ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n) {
        acc.append(tmp, n);
        size_t p;
        while ((p = acc.find('\n')) != std::string::npos) { emit(acc.substr(0, p)); acc.erase(0, p + 1); }
    }
    if (!acc.empty()) emit(acc);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    if (track) { std::lock_guard<std::mutex> lk(g.procMu); g.hProc = nullptr; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
    return code;
}

static void KillCurrent() {
    std::lock_guard<std::mutex> lk(g.procMu);
    if (g.hProc) TerminateProcess(g.hProc, 1);
}

struct Job {
    std::wstring exe, outDir, suffix, albedo, normal;
    double blend = 0; bool hdr = true; int gpu = 0; bool guides = false;
    std::vector<std::wstring> in;
};

static void WorkerMain(Job j) {
    int ok = 0, fail = 0;
    size_t n = j.in.size();
    for (size_t i = 0; i < n; ++i) {
        if (g.cancel) break;
        const std::wstring& in = j.in[i];
        Post(WM_APP_TEXT, 0, L"Processing " + std::to_wstring(i + 1) + L" of " + std::to_wstring(n) + L": " + FileName(in));
        Post(WM_APP_STATUS, i, L"Processing\u2026");

        std::wstring out = BuildOut(in, j.outDir, j.suffix);
        if (SamePath(in, out)) {   // the CLI deletes the output path first, so never let it equal the input
            Post(WM_APP_STATUS, i, L"Failed");
            Post(WM_APP_LOG, 0, L"Skipped " + FileName(in) +
                 L": the output path would overwrite the input. Set a suffix or another output folder.");
            ++fail; continue;
        }
        SHCreateDirectoryExW(nullptr, ParentDir(out).c_str(), nullptr);
        DeleteFileW(out.c_str());   // so a stale file from an earlier run cannot be mistaken for success

        wchar_t bl[16]; swprintf_s(bl, L"%.2f", j.blend);
        std::vector<std::wstring> args = { L"-v", L"2", L"-i", in, L"-o", out, L"-b", bl,
                                           L"-hdr", j.hdr ? L"1" : L"0", L"-gpu", std::to_wstring(j.gpu) };
        if (j.guides && !j.albedo.empty()) { args.push_back(L"-a"); args.push_back(j.albedo); }
        if (j.guides && !j.normal.empty()) { args.push_back(L"-n"); args.push_back(j.normal); }

        Post(WM_APP_LOG, 0, L"--- " + FileName(in) + L" ---");
        bool sawErr = false;
        DWORD code = RunProcess(j.exe, args, [](const std::wstring& l) { Post(WM_APP_LOG, 0, l); }, sawErr, true);
        if (g.cancel) { Post(WM_APP_STATUS, i, L"Cancelled"); break; }

        if (code == 0 && !sawErr && FileExists(out)) { Post(WM_APP_STATUS, i, L"Done"); ++ok; }
        else { Post(WM_APP_STATUS, i, L"Failed"); ++fail; }
        PostMessageW(g.hwnd, WM_APP_PROGRESS, (WPARAM)((i + 1) * 100 / n), 0);
    }
    PostMessageW(g.hwnd, WM_APP_DONE, (WPARAM)ok, (LPARAM)fail);
}

// ------------------------------------------------------------------ UI actions
static void AppendLog(const std::wstring& s) {
    int n = GetWindowTextLengthW(g.eLog);
    if (n > 150000) { SetWindowTextW(g.eLog, L""); n = 0; }
    SendMessageW(g.eLog, EM_SETSEL, n, n);
    std::wstring t = s + L"\r\n";
    SendMessageW(g.eLog, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
    SendMessageW(g.eLog, EM_SCROLLCARET, 0, 0);
}

static void UpdateUi() {
    bool busy = g.busy, single = g.items.size() == 1;
    ShowWindow(g.list, g.items.empty() ? SW_HIDE : SW_SHOW);
    for (HWND h : { g.bAdd, g.bRemove, g.bClear }) EnableWindow(h, !busy);
    for (HWND h : { g.eOutDir, g.bOutBrowse, g.eSuffix, g.sBlend, g.bHdr, g.eGpu, g.bDetect, g.eExe, g.bExeB })
        EnableWindow(h, !busy);
    for (HWND h : { g.eAlbedo, g.bAlbedoB, g.eNormal, g.bNormalB }) EnableWindow(h, !busy && single);
    EnableWindow(g.bStart, !busy);
    EnableWindow(g.bCancel, busy);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void AddPath(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) AddPath(p + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return;
    }
    if (!HasImageExt(p)) return;
    for (auto& it : g.items) if (_wcsicmp(it.path.c_str(), p.c_str()) == 0) return;
    g.items.push_back(Item{ p });
    SendMessageW(g.list, LB_ADDSTRING, 0, (LPARAM)L"");
}

static const wchar_t kImgFilter[] = L"Images\0*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.exr;*.bmp;*.tga;*.hdr\0All files\0*.*\0";
static const wchar_t kExeFilter[] = L"Denoiser\0Denoiser.exe\0Executables\0*.exe\0";

static std::vector<std::wstring> PickFiles(bool multi, const wchar_t* title, const wchar_t* filter) {
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW o{}; o.lStructSize = sizeof(o); o.hwndOwner = g.hwnd; o.lpstrFilter = filter;
    o.lpstrFile = buf.data(); o.nMaxFile = (DWORD)buf.size(); o.lpstrTitle = title;
    o.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (multi ? OFN_ALLOWMULTISELECT : 0);
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&o)) return out;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!multi || *p == 0) { out.push_back(first); return out; }
    while (*p) { std::wstring nme = p; out.push_back(first + L"\\" + nme); p += nme.size() + 1; }
    return out;
}

static std::wstring PickFolder() {
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD o = 0; d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(d->Show(g.hwnd))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(d->GetResult(&it))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
                it->Release();
            }
        }
        d->Release();
    }
    return out;
}

static void RemoveSelected() {
    int cnt = (int)SendMessageW(g.list, LB_GETSELCOUNT, 0, 0);
    if (cnt <= 0) return;
    std::vector<int> idx(cnt);
    SendMessageW(g.list, LB_GETSELITEMS, cnt, (LPARAM)idx.data());
    std::sort(idx.begin(), idx.end(), std::greater<int>());
    for (int i : idx) {
        if (i < 0 || i >= (int)g.items.size()) continue;
        g.items.erase(g.items.begin() + i);
        SendMessageW(g.list, LB_DELETESTRING, i, 0);
    }
    UpdateUi();
}

static void OnStart() {
    SaveSettings();
    std::wstring exe = ResolveExe();
    if (exe.empty()) {
        MessageBoxW(g.hwnd, L"Denoiser.exe was not found.\nPlace it next to this program or set its path in the Engine field.",
                    L"OptiX AI Denoiser", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g.items.empty()) {
        MessageBoxW(g.hwnd, L"Add at least one image first.", L"OptiX AI Denoiser", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Job j;
    j.exe = exe; j.outDir = Trim(GetText(g.eOutDir)); j.suffix = GetText(g.eSuffix);
    j.blend = g.blend; j.hdr = g.hdr; j.gpu = ParseGpu();
    j.guides = g.items.size() == 1;
    j.albedo = Trim(GetText(g.eAlbedo)); j.normal = Trim(GetText(g.eNormal));
    if (j.guides && !j.normal.empty() && j.albedo.empty()) {
        MessageBoxW(g.hwnd, L"A normal image requires an albedo image as well.", L"OptiX AI Denoiser", MB_OK | MB_ICONINFORMATION);
        return;
    }
    for (auto& it : g.items) { j.in.push_back(it.path); it.status = L"Queued"; }
    InvalidateRect(g.list, nullptr, FALSE);

    if (g.worker.joinable()) g.worker.join();
    g.cancel = false; g.busy = true; g.progress = 0; g.anim = 0;
    g.indet = g.items.size() == 1;
    g.statusText = L"Starting\u2026";
    SetTimer(g.hwnd, 1, 30, nullptr);
    UpdateUi();
    g.worker = std::thread(WorkerMain, std::move(j));
}

static void OnDetect() {
    std::wstring exe = ResolveExe();
    if (exe.empty()) { MessageBoxW(g.hwnd, L"Locate Denoiser.exe first.", L"Detect GPUs", MB_OK | MB_ICONINFORMATION); return; }
    std::thread([exe] {
        bool se = false;
        RunProcess(exe, {}, [](const std::wstring& l) {
            if (l.find(L"GPU ") != std::wstring::npos || l.find(L"CUDA") != std::wstring::npos ||
                l.find(L"ERROR") != std::wstring::npos) Post(WM_APP_LOG, 0, l);
        }, se, false);
    }).detach();
}

static void RefreshDpi() {
    MakeFonts(); ApplyFonts();
    SendMessageW(g.list, LB_SETITEMHEIGHT, 0, S(34));
    Layout();
}

// ------------------------------------------------------------------ window procedure
static void CreateControls() {
    auto btn = [&](int id, const wchar_t* t) {
        HWND h = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                 g.hwnd, (HMENU)(INT_PTR)id, g.hinst, nullptr);
        SetWindowSubclass(h, BtnSub, 1, 0);
        return h;
    };
    auto edit = [&](int id, DWORD extra = 0) {
        return CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra, 0, 0, 10, 10,
                               g.hwnd, (HMENU)(INT_PTR)id, g.hinst, nullptr);
    };
    g.list = CreateWindowExW(WS_EX_ACCEPTFILES, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_EXTENDEDSEL |
                             LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, g.hwnd, (HMENU)(INT_PTR)ID_LIST, g.hinst, nullptr);
    SetWindowSubclass(g.list, ListSub, 1, 0);
    SetWindowTheme(g.list, L"DarkMode_Explorer", nullptr);

    g.bAdd = btn(ID_ADD, L"Add images\u2026");
    g.bRemove = btn(ID_REMOVE, L"Remove");
    g.bClear = btn(ID_CLEAR, L"Clear");
    g.eOutDir = edit(ID_OUTDIR);  g.bOutBrowse = btn(ID_OUTBROWSE, L"\u2026");
    g.eSuffix = edit(ID_SUFFIX);
    g.sBlend = CreateWindowExW(0, L"DNSlider", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, g.hwnd,
                               (HMENU)(INT_PTR)ID_BLEND, g.hinst, nullptr);
    g.bHdr = btn(ID_HDR, L"HDR");
    g.eGpu = edit(ID_GPU, ES_NUMBER);  g.bDetect = btn(ID_DETECT, L"Detect GPUs");
    g.eAlbedo = edit(ID_ALBEDO);  g.bAlbedoB = btn(ID_ALBEDOB, L"\u2026");
    g.eNormal = edit(ID_NORMAL);  g.bNormalB = btn(ID_NORMALB, L"\u2026");
    g.eExe = edit(ID_EXE);        g.bExeB = btn(ID_EXEB, L"\u2026");
    g.eLog = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                             0, 0, 10, 10, g.hwnd, (HMENU)(INT_PTR)ID_LOG, g.hinst, nullptr);
    SetWindowTheme(g.eLog, L"DarkMode_Explorer", nullptr);
    g.bCancel = btn(ID_CANCEL, L"Cancel");
    g.bStart = btn(ID_START, L"DENOISE");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;
        BOOL on = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &on, sizeof(on));            // dark title bar
        COLORREF cap = cHeader, txt = cFg;
        DwmSetWindowAttribute(hwnd, 35, &cap, sizeof(cap));          // Windows 11: caption colour
        DwmSetWindowAttribute(hwnd, 36, &txt, sizeof(txt));
        MakeFonts();
        CreateControls();
        ApplyFonts();
        LoadSettings();
        ResolveExe();
        DragAcceptFiles(hwnd, TRUE);
        g.ready = true;
        Layout();
        UpdateUi();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        RECT r{ 0, 0, S(940), S(730) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, g.dpi);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Layout();
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        auto* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RefreshDpi();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintAll(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX: {
        HDC d = (HDC)wp;
        SetBkColor(d, cField);
        SetTextColor(d, IsWindowEnabled((HWND)lp) ? cFg : cDim);
        return (LRESULT)g.brField;
    }
    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi->CtlType == ODT_LISTBOX) { mi->itemHeight = S(34); return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_BUTTON) { DrawButton(di); return TRUE; }
        if (di->CtlType == ODT_LISTBOX) { DrawListItem(di); return TRUE; }
        break;
    }
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        if (!g.busy) {
            UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(hd, i, nullptr, 0);
                std::wstring p(len + 1, L'\0');
                DragQueryFileW(hd, i, &p[0], len + 1);
                p.resize(len);
                AddPath(p);
            }
            UpdateUi();
        }
        DragFinish(hd);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) { InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (code != BN_CLICKED) break;
        switch (id) {
        case ID_ADD:
            for (auto& f : PickFiles(true, L"Select images to denoise", kImgFilter)) AddPath(f);
            UpdateUi();
            break;
        case ID_REMOVE: RemoveSelected(); break;
        case ID_CLEAR:
            g.items.clear();
            SendMessageW(g.list, LB_RESETCONTENT, 0, 0);
            UpdateUi();
            break;
        case ID_OUTBROWSE: { auto f = PickFolder(); if (!f.empty()) SetWindowTextW(g.eOutDir, f.c_str()); break; }
        case ID_HDR: g.hdr = !g.hdr; InvalidateRect(g.bHdr, nullptr, FALSE); break;
        case ID_DETECT: OnDetect(); break;
        case ID_ALBEDOB: { auto f = PickFiles(false, L"Select albedo image", kImgFilter);
                           if (!f.empty()) SetWindowTextW(g.eAlbedo, f[0].c_str()); break; }
        case ID_NORMALB: { auto f = PickFiles(false, L"Select normal image", kImgFilter);
                           if (!f.empty()) SetWindowTextW(g.eNormal, f[0].c_str()); break; }
        case ID_EXEB: { auto f = PickFiles(false, L"Locate Denoiser.exe", kExeFilter);
                        if (!f.empty()) SetWindowTextW(g.eExe, f[0].c_str()); break; }
        case ID_START: OnStart(); break;
        case ID_CANCEL:
            g.cancel = true;
            KillCurrent();
            g.statusText = L"Cancelling\u2026";
            InvalidateRect(hwnd, &g.rStatus, FALSE);
            break;
        }
        return 0;
    }
    case WM_TIMER:
        if (g.busy && g.indet) { g.anim += S(8); InvalidateRect(hwnd, &g.rProg, FALSE); }
        return 0;
    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        AppendLog(*s);
        return 0;
    }
    case WM_APP_STATUS: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        if (wp < g.items.size()) { g.items[wp].status = *s; InvalidateRect(g.list, nullptr, FALSE); }
        return 0;
    }
    case WM_APP_TEXT: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        g.statusText = *s;
        InvalidateRect(hwnd, &g.rStatus, FALSE);
        return 0;
    }
    case WM_APP_PROGRESS:
        g.indet = false; g.progress = (int)wp;
        InvalidateRect(hwnd, &g.rProg, FALSE);
        return 0;
    case WM_APP_DONE: {
        if (g.worker.joinable()) g.worker.join();
        int ok = (int)wp, fail = (int)lp;
        bool cancelled = g.cancel;
        g.busy = false; g.indet = false;
        KillTimer(hwnd, 1);
        if (cancelled) {
            g.statusText = L"Cancelled";
            for (auto& it : g.items)
                if (it.status == L"Queued" || it.status.rfind(L"Processing", 0) == 0) it.status = L"Cancelled";
        } else {
            g.progress = 100;
            g.statusText = fail == 0 ? L"Finished: " + std::to_wstring(ok) + L" image(s) denoised"
                                     : L"Finished: " + std::to_wstring(ok) + L" done, " + std::to_wstring(fail) + L" failed";
        }
        InvalidateRect(g.list, nullptr, FALSE);
        UpdateUi();
        return 0;
    }
    case WM_CLOSE:
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g.cancel = true;
        KillCurrent();
        if (g.worker.joinable()) g.worker.join();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ entry point
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ic{ sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);

    g.hinst = hInst;
    g.dpi = (int)GetDpiForSystem();
    g.brField = CreateSolidBrush(cField);

    // Job object: any Denoiser.exe we launch is killed if this process goes away.
    g.job = CreateJobObjectW(nullptr, nullptr);
    if (g.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g.job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }

    WNDCLASSEXW sc{ sizeof(sc) };
    sc.lpfnWndProc = SliderProc; sc.hInstance = hInst; sc.lpszClassName = L"DNSlider";
    sc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&sc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"OptiXDenoiserMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT r{ 0, 0, S(1000), S(760) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g.dpi);
    HWND hwnd = CreateWindowExW(0, L"OptiXDenoiserMain", L"OptiX AI Denoiser", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    int actual = (int)GetDpiForWindow(hwnd);
    if (actual != g.dpi) { g.dpi = actual; RefreshDpi(); }
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (g.job) CloseHandle(g.job);
    CoUninitialize();
    return (int)m.wParam;
}
