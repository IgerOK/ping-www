/*
 * PingWWW.c — мини-индикатор интернета (аналог PingWWW.ahk)
 * Компилятор: Pelles C 14.50 (также MinGW / MSVC)
 *
 * Проект: Win32 Application (GUI)
 * Subsystem: Windows, Entry point: (авто)
 * Libraries: kernel32.lib user32.lib gdi32.lib comctl32.lib wininet.lib
 * Defines:   UNICODE _UNICODE
 *
 * Файл сохранять в UTF-8 with BOM.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601        /* Windows 7+ */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <wininet.h>
#include <math.h>

/* ---- Параметры -------------------------------------------------- */
#define TIMER_CHECK         1
#define WM_APP_CHECK_DONE   (WM_APP + 1)

static const int SIZES[] = { 30, 50, 70 };
#define SIZES_COUNT (int)(sizeof(SIZES) / sizeof(SIZES[0]))

static const int   INTERVAL_OFFLINE = 1000;   /* мс */
static const int   INTERVAL_ONLINE  = 5000;   /* мс */
static const DWORD HTTP_TIMEOUT     = 1000;   /* мс */

static const wchar_t* CHECK_URL =
    L"http://www.msftconnecttest.com/connecttest.txt";

/* ---- Состояние -------------------------------------------------- */
static int   g_sizeLevel   = 1;               /* 50 px */
static BOOL  g_transparent = TRUE;
static BOOL  g_online      = FALSE;
static BOOL  g_dragging    = FALSE;
static BOOL  g_tracking    = FALSE;
static BOOL  g_tipShown    = FALSE;

static HWND       g_hTip = NULL;
static TOOLINFOW  g_ti   = {0};
static wchar_t    g_tipText[128];

static POINT      g_dragStart;
static RECT       g_dragOrigin;

/* ---- Утилиты ---------------------------------------------------- */
static COLORREF CurrentColor(void)
{
    return g_online ? RGB(0, 180, 0) : RGB(220, 30, 30);
}

/* ---- Отрисовка с попиксельной альфой ---------------------------- */
static void RenderWindow(HWND hwnd, int size)
{
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = size;
    bmi.bmiHeader.biHeight      = -size;     /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   bits   = NULL;
    HBITMAP hbm    = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS,
                                      &bits, NULL, 0);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    COLORREF col       = CurrentColor();
    int      baseAlpha = g_transparent ? 128 : 255;

    BYTE r = GetRValue(col);
    BYTE g = GetGValue(col);
    BYTE b = GetBValue(col);

    float cx = size / 2.0f;
    float cy = size / 2.0f;
    float R  = size / 2.0f;

    DWORD* px = (DWORD*)bits;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = (x + 0.5f) - cx;
            float dy = (y + 0.5f) - cy;
            float d  = sqrtf(dx * dx + dy * dy);

            /* линейный антиалиасинг: пиксель покрыт пропорционально */
            float a = R + 0.5f - d;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;

            BYTE alpha = (BYTE)(a * baseAlpha);

            /* premultiplied alpha — требование ULW_ALPHA */
            BYTE pr = (BYTE)(r * alpha / 255);
            BYTE pg = (BYTE)(g * alpha / 255);
            BYTE pb = (BYTE)(b * alpha / 255);

            px[y * size + x] = ((DWORD)alpha << 24)
                             | ((DWORD)pr    << 16)
                             | ((DWORD)pg    <<  8)
                             |  (DWORD)pb;
        }
    }

    POINT ptSrc = { 0, 0 };
    SIZE  sz    = { size, size };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(hwnd, hdcScreen, NULL, &sz,
                        hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

/* Позиционирование с сохранением центра и прижатием к рабочей области */
static void ApplySize(HWND hwnd)
{
    int size = SIZES[g_sizeLevel];

    RECT rc;
    GetWindowRect(hwnd, &rc);
    int cx = (rc.left + rc.right)  / 2;
    int cy = (rc.top  + rc.bottom) / 2;
    int x  = cx - size / 2;
    int y  = cy - size / 2;

    HMONITOR hm = MonitorFromPoint((POINT){ cx, cy }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);

    int left  = mi.rcWork.left,  top    = mi.rcWork.top;
    int right = mi.rcWork.right, bottom = mi.rcWork.bottom;

    if (size >= right - left)   x = left;
    else if (x < left)          x = left;
    else if (x + size > right)  x = right - size;

    if (size >= bottom - top)   y = top;
    else if (y < top)           y = top;
    else if (y + size > bottom) y = bottom - size;

    SetWindowPos(hwnd, NULL, x, y, size, size,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    RenderWindow(hwnd, size);
}

/* ---- Тултип ----------------------------------------------------- */
static void ShowTip(HWND hwnd)
{
    if (g_tipShown || !g_hTip) return;

    wsprintfW(g_tipText,
              L"Author: IgerOK\nLicense: MIT\nStatus: %s",
              g_online ? L"Online" : L"Offline");
    SendMessageW(g_hTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&g_ti);

    RECT rc;
    GetWindowRect(hwnd, &rc);
    SendMessageW(g_hTip, TTM_TRACKPOSITION, 0,
                 MAKELPARAM(rc.right + 4, rc.top + 2));
    SendMessageW(g_hTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&g_ti);
    g_tipShown = TRUE;
}

static void HideTip(void)
{
    if (!g_tipShown || !g_hTip) return;
    SendMessageW(g_hTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&g_ti);
    g_tipShown = FALSE;
}

/* ---- Поток HTTP-проверки --------------------------------------- */
static DWORD WINAPI CheckThread(LPVOID param)
{
    HWND hwnd = (HWND)param;
    BOOL ok   = FALSE;

    HINTERNET hInet = InternetOpenW(L"PingWWW/1.0",
                                    INTERNET_OPEN_TYPE_PRECONFIG,
                                    NULL, NULL, 0);
    if (hInet) {
        DWORD t = HTTP_TIMEOUT;
        InternetSetOptionW(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
        InternetSetOptionW(hInet, INTERNET_OPTION_SEND_TIMEOUT,    &t, sizeof(t));
        InternetSetOptionW(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));

        HINTERNET hUrl = InternetOpenUrlW(
            hInet, CHECK_URL, NULL, 0,
            INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD, 0);

        if (hUrl) {
            DWORD status = 0, len = sizeof(status);
            if (HttpQueryInfoW(hUrl,
                               HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                               &status, &len, NULL))
            {
                ok = (status == 200);
            }
            InternetCloseHandle(hUrl);
        }
        InternetCloseHandle(hInet);
    }

    PostMessageW(hwnd, WM_APP_CHECK_DONE, ok ? 1 : 0, 0);
    return 0;
}

static void StartCheck(HWND hwnd)
{
    CloseHandle(CreateThread(NULL, 0, CheckThread, hwnd, 0, NULL));
}

/* ---- Оконная процедура ----------------------------------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCTW)lp)->hInstance;

        g_hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                 WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 hwnd, NULL, hInst, NULL);
        if (g_hTip) {
            SetWindowPos(g_hTip, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(g_hTip, TTM_SETMAXTIPWIDTH, 0, 400);

            g_ti.cbSize   = sizeof(g_ti);
            g_ti.uFlags   = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
            g_ti.hwnd     = hwnd;
            g_ti.uId      = (UINT_PTR)hwnd;
            g_ti.lpszText = L"";
            SendMessageW(g_hTip, TTM_ADDTOOLW, 0, (LPARAM)&g_ti);
        }

        StartCheck(hwnd);
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_CHECK) {
            KillTimer(hwnd, TIMER_CHECK);
            StartCheck(hwnd);
        }
        return 0;

    case WM_APP_CHECK_DONE: {
        BOOL ok = (wp != 0);
        if (ok != g_online) {
            g_online = ok;
            RenderWindow(hwnd, SIZES[g_sizeLevel]);
            if (g_tipShown) { HideTip(); ShowTip(hwnd); }
        }
        SetTimer(hwnd, TIMER_CHECK,
                 g_online ? INTERVAL_ONLINE : INTERVAL_OFFLINE, NULL);
        return 0;
    }

    case WM_LBUTTONDOWN:
        g_dragging = TRUE;
        SetFocus(hwnd);
        SetCapture(hwnd);
        GetCursorPos(&g_dragStart);
        GetWindowRect(hwnd, &g_dragOrigin);
        HideTip();
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = FALSE;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        g_transparent = !g_transparent;
        RenderWindow(hwnd, SIZES[g_sizeLevel]);
        return 0;

    case WM_RBUTTONUP:
        g_sizeLevel = (g_sizeLevel + 1) % SIZES_COUNT;
        ApplySize(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            int dx = pt.x - g_dragStart.x;
            int dy = pt.y - g_dragStart.y;
            SetWindowPos(hwnd, NULL,
                         g_dragOrigin.left + dx,
                         g_dragOrigin.top  + dy,
                         0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (!g_tracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize      = sizeof(tme);
            tme.dwFlags     = TME_LEAVE;
            tme.hwndTrack   = hwnd;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
            g_tracking = TRUE;
        }
        ShowTip(hwnd);
        return 0;

    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        HideTip();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_dragging) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g_hTip) DestroyWindow(g_hTip);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- Точка входа ------------------------------------------------ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR cmdLine, int nShow)
{
    (void)hPrev; (void)cmdLine; (void)nShow;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_HAND);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"PingWWWClass";
    if (!RegisterClassExW(&wc)) return 1;

    int size = SIZES[g_sizeLevel];

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"PingWWWClass", L"WWW",
        WS_POPUP,
        50, 50, size, size,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    RenderWindow(hwnd, size);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
