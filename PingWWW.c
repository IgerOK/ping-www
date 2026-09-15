/*
PingWWW.c — мини-индикатор интернета (аналог PingWWW.ahk)
Компилятор: Pelles C 14.50 (также MinGW / MSVC)
Проект: Win32 Application (GUI)
Subsystem: Windows, Entry point: (авто)
Libraries: kernel32.lib user32.lib gdi32.lib comctl32.lib wininet.lib advapi32.lib
Defines:   UNICODE _UNICODE
Файл сохранять в UTF-8 with BOM.
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
static const wchar_t* REPO_URL  = L"https://github.com/IgerOK/ping-www";

/* ---- Состояние -------------------------------------------------- */
static int   g_sizeLevel   = 1;               /* 50 px */
static BOOL  g_transparent = TRUE;
static BOOL  g_online      = FALSE;
static BOOL  g_firstCheckDone = FALSE;
static BOOL  g_dragging    = FALSE;

/* Всплывающее окно при перетаскивании */
static HWND      g_hPopup     = NULL;
static BOOL      g_popupShown = FALSE;
static ULONGLONG g_startTick  = 0;
static POINT     g_dragStart;
static RECT      g_dragOrigin;

/* ---- Утилиты ---------------------------------------------------- */
static COLORREF CurrentColor(void)
{
    if (!g_firstCheckDone) return RGB(140, 140, 140); /* Серый: идет первая проверка */
    return g_online ? RGB(0, 180, 0) : RGB(220, 30, 30);
}

/* ---- Реестр: сохранение / загрузка состояния --------------------- */
#define REG_KEY  L"Software\\PingWWW"

static void SaveState(HWND hwnd)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY,
                        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RECT rc;
        GetWindowRect(hwnd, &rc);
        DWORD x = (DWORD)rc.left;
        DWORD y = (DWORD)rc.top;
        DWORD sizeLevel = (DWORD)g_sizeLevel;
        DWORD transparent = g_transparent ? 1 : 0;

        RegSetValueExW(hKey, L"PosX", 0, REG_DWORD, (const BYTE*)&x, sizeof(x));
        RegSetValueExW(hKey, L"PosY", 0, REG_DWORD, (const BYTE*)&y, sizeof(y));
        RegSetValueExW(hKey, L"SizeLevel", 0, REG_DWORD, (const BYTE*)&sizeLevel, sizeof(sizeLevel));
        RegSetValueExW(hKey, L"Transparent", 0, REG_DWORD, (const BYTE*)&transparent, sizeof(transparent));
        RegCloseKey(hKey);
    }
}

static BOOL LoadState(int* x, int* y)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY,
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;

    DWORD xVal = 0, yVal = 0, sizeVal = 1, transVal = 1;
    DWORD size = sizeof(DWORD), type = 0;
    
    BOOL okX = (RegQueryValueExW(hKey, L"PosX", NULL, &type, (BYTE*)&xVal, &size) == ERROR_SUCCESS && type == REG_DWORD);
    size = sizeof(DWORD); type = 0;
    BOOL okY = (RegQueryValueExW(hKey, L"PosY", NULL, &type, (BYTE*)&yVal, &size) == ERROR_SUCCESS && type == REG_DWORD);
    
    /* Читаем размер (защита от некорректных значений в реестре) */
    size = sizeof(DWORD); type = 0;
    if (RegQueryValueExW(hKey, L"SizeLevel", NULL, &type, (BYTE*)&sizeVal, &size) == ERROR_SUCCESS && type == REG_DWORD) {
        if (sizeVal < (DWORD)SIZES_COUNT) g_sizeLevel = (int)sizeVal;
    }

    /* Читаем прозрачность */
    size = sizeof(DWORD); type = 0;
    if (RegQueryValueExW(hKey, L"Transparent", NULL, &type, (BYTE*)&transVal, &size) == ERROR_SUCCESS && type == REG_DWORD) {
        g_transparent = (transVal != 0) ? TRUE : FALSE;
    }

    RegCloseKey(hKey);

    if (okX && okY) {
        *x = (int)xVal;
        *y = (int)yVal;
        return TRUE;
    }
    return FALSE;
}

static void ClampPosition(int* x, int* y, int size)
{
    POINT pt = { *x + size / 2, *y + size / 2 };
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);

    if (size >= mi.rcWork.right - mi.rcWork.left)
        *x = mi.rcWork.left;
    else if (*x < mi.rcWork.left)
        *x = mi.rcWork.left;
    else if (*x + size > mi.rcWork.right)
        *x = mi.rcWork.right - size;

    if (size >= mi.rcWork.bottom - mi.rcWork.top)
        *y = mi.rcWork.top;
    else if (*y < mi.rcWork.top)
        *y = mi.rcWork.top;
    else if (*y + size > mi.rcWork.bottom)
        *y = mi.rcWork.bottom - size;
}

/* ---- Отрисовка индикатора с попиксельной альфой ------------------ */
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

            float a = R + 0.5f - d;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;

            BYTE alpha = (BYTE)(a * baseAlpha);

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

/* Покрытие скруглённого прямоугольника (0..1) для антиалиасинга */
static float RRectCoverage(float px, float py, float w, float h, float r)
{
    float nx = px, ny = py;
    if (nx < r)     nx = r;     else if (nx > w - r) nx = w - r;
    if (ny < r)     ny = r;     else if (ny > h - r) ny = h - r;

    float dx = px - nx, dy = py - ny;
    float d  = sqrtf(dx * dx + dy * dy);

    float a = r + 0.5f - d;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return a;
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

    ClampPosition(&x, &y, size);

    SetWindowPos(hwnd, NULL, x, y, size, size,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    RenderWindow(hwnd, size);
}

/* ---- Всплывающее окно при перетаскивании ------------------------ */
static HFONT MakeFont(int height, int weight)
{
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void RenderPopup(HWND hwnd)
{
    wchar_t up[64];
    ULONGLONG mins = (GetTickCount64() - g_startTick) / 60000ULL;
    wsprintfW(up, L"Uptime: %02u:%02u",
              (UINT)(mins / 60), (UINT)(mins % 60));

    static const wchar_t* HINT_LMB = L"2×ЛКМ — прозрачность";
    static const wchar_t* HINT_ESC = L"Нажмите Esc для выхода из программы";

    wchar_t hintRmb[64];
    int n = wsprintfW(hintRmb, L"ПКМ — размер (");
    for (int i = 0; i < SIZES_COUNT; i++)
        n += wsprintfW(hintRmb + n, L"%s%d", i ? L"/" : L"", SIZES[i]);
    wsprintfW(hintRmb + n, L")");

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    HFONT fUrl  = MakeFont(-15, FW_NORMAL);
    HFONT fUp   = MakeFont(-18, FW_SEMIBOLD);
    HFONT fHint = MakeFont(-13, FW_NORMAL);

    SelectObject(hdcMem, fUrl);
    SIZE sUrl;
    GetTextExtentPoint32W(hdcMem, REPO_URL, lstrlenW(REPO_URL), &sUrl);

    SelectObject(hdcMem, fUp);
    SIZE sUp;
    GetTextExtentPoint32W(hdcMem, up, lstrlenW(up), &sUp);

    SelectObject(hdcMem, fHint);
    SIZE sH1, sH2, sH3;
    GetTextExtentPoint32W(hdcMem, HINT_LMB, lstrlenW(HINT_LMB), &sH1);
    GetTextExtentPoint32W(hdcMem, hintRmb,  lstrlenW(hintRmb),  &sH2);
    GetTextExtentPoint32W(hdcMem, HINT_ESC, lstrlenW(HINT_ESC), &sH3);

    const int padX = 12, padY = 10, gap = 6, sepGap = 8, radius = 9;

    int w = sUrl.cx;
    if (sUp.cx  > w) w = sUp.cx;
    if (sH1.cx  > w) w = sH1.cx;
    if (sH2.cx  > w) w = sH2.cx;
    if (sH3.cx  > w) w = sH3.cx;
    w += padX * 2;

    int yUrl = padY;
    int yUp  = yUrl + sUrl.cy + gap;
    int ySep = yUp  + sUp.cy  + sepGap;
    int yH1  = ySep + 1       + sepGap;
    int yH2  = yH1  + sH1.cy  + gap;
    int yH3  = yH2  + sH2.cy  + gap;
    int h    = yH3  + sH3.cy  + padY;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;        /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   bits   = NULL;
    HBITMAP hbm    = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS,
                                      &bits, NULL, 0);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    RECT rcFill = { 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 30));
    FillRect(hdcMem, &rcFill, bg);
    DeleteObject(bg);

    RECT rcSep = { padX, ySep, w - padX, ySep + 1 };
    HBRUSH sep = CreateSolidBrush(RGB(60, 60, 66));
    FillRect(hdcMem, &rcSep, sep);
    DeleteObject(sep);

    SetBkMode(hdcMem, TRANSPARENT);

    SelectObject(hdcMem, fUrl);
    SetTextColor(hdcMem, RGB(140, 180, 255));
    TextOutW(hdcMem, padX, yUrl, REPO_URL, lstrlenW(REPO_URL));

    SelectObject(hdcMem, fUp);
    SetTextColor(hdcMem, RGB(240, 240, 240));
    TextOutW(hdcMem, padX, yUp, up, lstrlenW(up));

    SelectObject(hdcMem, fHint);
    SetTextColor(hdcMem, RGB(160, 165, 175));
    TextOutW(hdcMem, padX, yH1, HINT_LMB, lstrlenW(HINT_LMB));
    TextOutW(hdcMem, padX, yH2, hintRmb,  lstrlenW(hintRmb));
    TextOutW(hdcMem, padX, yH3, HINT_ESC, lstrlenW(HINT_ESC));

    DWORD* px = (DWORD*)bits;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            DWORD  c = px[y * w + x];
            float  a = RRectCoverage(x + 0.5f, y + 0.5f,
                                     (float)w, (float)h, (float)radius);
            if (a <= 0.0f) { px[y * w + x] = 0; continue; }

            int A = (a >= 1.0f) ? 255 : (int)(a * 255.0f + 0.5f);
            int r = (int)((c >> 16) & 0xFF);
            int g = (int)((c >>  8) & 0xFF);
            int b = (int)( c        & 0xFF);

            px[y * w + x] = ((DWORD)A                      << 24)
                          | ((DWORD)(r * A / 255)          << 16)
                          | ((DWORD)(g * A / 255)          <<  8)
                          |  (DWORD)(b * A / 255);
        }
    }

    POINT ptSrc = { 0, 0 };
    SIZE  sz    = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(hwnd, hdcScreen, NULL, &sz,
                        hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteObject(fUrl);
    DeleteObject(fUp);
    DeleteObject(fHint);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

static void MovePopup(HWND hwndMain)
{
    if (!g_hPopup) return;

    RECT rm, rp;
    GetWindowRect(hwndMain, &rm);
    GetWindowRect(g_hPopup, &rp);
    int w = rp.right - rp.left;
    int h = rp.bottom - rp.top;

    int x = rm.right + 6;
    int y = rm.top;

    HMONITOR hm = MonitorFromWindow(hwndMain, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);

    if (x + w > mi.rcWork.right) x = rm.left - 6 - w;
    if (x < mi.rcWork.left)      x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (y < mi.rcWork.top)        y = mi.rcWork.top;

    SetWindowPos(g_hPopup, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ShowPopup(HWND hwndMain)
{
    if (!g_hPopup || g_popupShown) return;
    RenderPopup(g_hPopup);
    MovePopup(hwndMain);
    ShowWindow(g_hPopup, SW_SHOWNOACTIVATE);
    g_popupShown = TRUE;
}

static void HidePopup(void)
{
    if (!g_hPopup || !g_popupShown) return;
    ShowWindow(g_hPopup, SW_HIDE);
    g_popupShown = FALSE;
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

        g_hPopup = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"PingWWWTipClass", NULL, WS_POPUP,
            0, 0, 0, 0,
            hwnd, NULL, hInst, NULL);

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
        g_firstCheckDone = TRUE;
        
        if (ok != g_online) {
            g_online = ok;
        }
        RenderWindow(hwnd, SIZES[g_sizeLevel]);
        
        if (g_popupShown) {
            RenderPopup(g_hPopup);
            MovePopup(hwnd);
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
        ShowPopup(hwnd);
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = FALSE;
            ReleaseCapture();
        }
        HidePopup();
        return 0;

    case WM_CAPTURECHANGED:
        g_dragging = FALSE;
        HidePopup();
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
            MovePopup(hwnd);
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_dragging) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        SaveState(hwnd);          /* запомнить позицию, размер и прозрачность */
        if (g_hPopup) DestroyWindow(g_hPopup);
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

    /* Защита от запуска нескольких копий программы (Single Instance) */
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\PingWWW_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, 
                    L"Программа PingWWW уже запущена.\nЗакройте работающую копию перед запуском новой.",
                    L"PingWWW", 
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        if (hMutex) CloseHandle(hMutex);
        return 0; /* Завершаем вторую копию */
    }

    g_startTick = GetTickCount64();

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
    if (!RegisterClassExW(&wc)) { CloseHandle(hMutex); return 1; }

    WNDCLASSEXW wct;
    ZeroMemory(&wct, sizeof(wct));
    wct.cbSize        = sizeof(wct);
    wct.lpfnWndProc   = DefWindowProcW;
    wct.hInstance     = hInst;
    wct.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wct.lpszClassName = L"PingWWWTipClass";
    if (!RegisterClassExW(&wct)) { CloseHandle(hMutex); return 1; }

    int posX = 50, posY = 50;
    /* LoadState считывает X и Y, а также обновляет g_sizeLevel и g_transparent */
    LoadState(&posX, &posY);
    
    int size = SIZES[g_sizeLevel];
    ClampPosition(&posX, &posY, size);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"PingWWWClass", L"WWW",
        WS_POPUP,
        posX, posY, size, size,
        NULL, NULL, hInst, NULL);
        
    if (!hwnd) {
        CloseHandle(hMutex);
        return 1;
    }

    RenderWindow(hwnd, size);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* При нормальном завершении ОС освободит мьютекс сама, 
       но явное закрытие дескриптора — это good practice */
    CloseHandle(hMutex);
    return (int)msg.wParam;
}

