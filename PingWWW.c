/*
https://github.com/IgerOK/ping-www
PingWWW.c — мини-индикатор интернета
Компилятор: Pelles C 14.50 (также MinGW / MSVC)
Проект: Win32 Application (GUI)
Subsystem: Windows, Entry point: (авто)
Libraries: kernel32.lib user32.lib gdi32.lib comctl32.lib wininet.lib advapi32.lib iphlpapi.lib
Defines:   UNICODE _UNICODE
Файл сохранять в UTF-8 with BOM.
*/

// СТРОГО ДО ВСЕХ ИНКЛУДОВ: Установка таргетинга на Windows 7+ для доступа к NetIO API
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <commctrl.h>
#include <wchar.h>
#include <string.h>

// Порядок подключения заголовков сети важен для Pelles C
#include <iphlpapi.h>
#include <netioapi.h> 
#include <math.h>

// Константы окон и идентификаторы таймеров
#define WINDOW_CLASS_NAME L"PingWWW_Class"
#define POPUP_CLASS_NAME L"PingWWW_Popup_Class"
#define WM_APP_CHECK_DONE (WM_APP + 1)
#define ID_TIMER_CHECK 1
#define ID_TIMER_POPUP 2
#define ID_TIMER_SPEED_TRACK 3

// Ключ реестра для сохранения состояния программы
#define REG_KEY_PATH L"Software\\PingWWW"

// Глобальные переменные состояния приложения
HWND g_hwndMain = NULL;
HWND g_hwndPopup = NULL;
BOOL g_isNetworkUp = TRUE;
BOOL g_isChecking = FALSE;
DWORD g_startTick = 0;

// Пользовательские настройки (сохраняются в реестре)
int g_posX = 50;
int g_posY = 50;
int g_sizeLevel = 1; // 0 = 30px, 1 = 50px, 2 = 70px
BOOL g_isTransparent = TRUE; // TRUE = 50%, FALSE = 100%

// Переменные для перетаскивания окна мышью
BOOL g_isDragging = FALSE;
POINT g_dragStartMouse;
POINT g_dragStartWindow;

// Дескрипторы графических объектов (шрифты)
HFONT g_hFontNormal = NULL;

// Глобальные переменные для учета сетевого трафика
ULONGLONG g_initialInBytes = 0;
ULONGLONG g_initialOutBytes = 0;

// Переменные для расчета мгновенной и чистой средней скорости
ULONGLONG g_lastInBytes = 0;
ULONGLONG g_lastOutBytes = 0;
double g_currentSpeedInKb = 0.0;
double g_currentSpeedOutKb = 0.0;
DWORD g_activeInSeconds = 0;
DWORD g_activeOutSeconds = 0;

// Массив доступных диаметров индикатора
const int g_sizes[] = { 30, 50, 70 };

// Определение прототипов функций
void LoadSettings(void);
void SaveSettings(void);
void ClampPositionToMonitor(int* x, int* y, int size);
void GetTotalNetworkBytes(ULONGLONG* inBytes, ULONGLONG* outBytes);
DWORD WINAPI NetworkCheckThread(LPVOID lpParam);
void UpdateMainLayeredWindow(void);
void UpdatePopupLayeredWindow(void);
void FormatSpeedString(wchar_t* buffer, size_t bufferSize, const wchar_t* prefix, double currentKb, double avgKb);
void RecreateFontsForDpi(HWND hwnd);

// Функция сбора сетевой статистики через классический IP Helper API (совместимый со всеми версиями SDK)
void GetTotalNetworkBytes(ULONGLONG* inBytes, ULONGLONG* outBytes) {
    *inBytes = 0;
    *outBytes = 0;
    
    ULONG dwSize = 0;
    if (GetIfTable(NULL, &dwSize, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
        MIB_IFTABLE* pIfTable = (MIB_IFTABLE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize);
        if (pIfTable) {
            if (GetIfTable(pIfTable, &dwSize, FALSE) == NO_ERROR) {
                for (ULONG i = 0; i < pIfTable->dwNumEntries; i++) {
                    if (pIfTable->table[i].dwType != IF_TYPE_SOFTWARE_LOOPBACK && 
                        pIfTable->table[i].dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL) {
                        *inBytes += pIfTable->table[i].dwInOctets;
                        *outBytes += pIfTable->table[i].dwOutOctets;
                    }
                }
            }
            HeapFree(GetProcessHeap(), 0, pIfTable);
        }
    }
}

// Загрузка настроек приложения из реестра Windows
void LoadSettings(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwType, dwSize, dwValue;
        
        dwSize = sizeof(dwValue);
        if (RegQueryValueExW(hKey, L"PosX", NULL, &dwType, (BYTE*)&dwValue, &dwSize) == ERROR_SUCCESS) {
            g_posX = (int)dwValue;
        }
        
        dwSize = sizeof(dwValue);
        if (RegQueryValueExW(hKey, L"PosY", NULL, &dwType, (BYTE*)&dwValue, &dwSize) == ERROR_SUCCESS) {
            g_posY = (int)dwValue;
        }
        
        dwSize = sizeof(dwValue);
        if (RegQueryValueExW(hKey, L"SizeLevel", NULL, &dwType, (BYTE*)&dwValue, &dwSize) == ERROR_SUCCESS) {
            g_sizeLevel = (int)dwValue;
            if (g_sizeLevel < 0 || g_sizeLevel > 2) g_sizeLevel = 1;
        }
        
        dwSize = sizeof(dwValue);
        if (RegQueryValueExW(hKey, L"Transparent", NULL, &dwType, (BYTE*)&dwValue, &dwSize) == ERROR_SUCCESS) {
            g_isTransparent = (BOOL)dwValue;
        }
        
        RegCloseKey(hKey);
    }
    ClampPositionToMonitor(&g_posX, &g_posY, g_sizes[g_sizeLevel]);
}

// Сохранение настроек приложения в реестр Windows
void SaveSettings(void) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD dwValue;
        
        dwValue = (DWORD)g_posX;
        RegSetValueExW(hKey, L"PosX", 0, REG_DWORD, (BYTE*)&dwValue, sizeof(dwValue));
        
        dwValue = (DWORD)g_posY;
        RegSetValueExW(hKey, L"PosY", 0, REG_DWORD, (BYTE*)&dwValue, sizeof(dwValue));
        
        dwValue = (DWORD)g_sizeLevel;
        RegSetValueExW(hKey, L"SizeLevel", 0, REG_DWORD, (BYTE*)&dwValue, sizeof(dwValue));
        
        dwValue = (DWORD)g_isTransparent;
        RegSetValueExW(hKey, L"Transparent", 0, REG_DWORD, (BYTE*)&dwValue, sizeof(dwValue));
        
        RegCloseKey(hKey);
    }
}

// Ограничение координат окна в пределах рабочей области текущего монитора
void ClampPositionToMonitor(int* x, int* y, int size) {
    POINT pt = { *x + size / 2, *y + size / 2 };
    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfoW(hMonitor, &mi)) {
        if (*x < mi.rcWork.left) *x = mi.rcWork.left;
        if (*x + size > mi.rcWork.right) *x = mi.rcWork.right - size;
        if (*y < mi.rcWork.top) *y = mi.rcWork.top;
        if (*y + size > mi.rcWork.bottom) *y = mi.rcWork.bottom - size;
    }
}

// Потоковая функция для проверки доступности сети Интернет
DWORD WINAPI NetworkCheckThread(LPVOID lpParam) {
    HWND hwnd = (HWND)lpParam;
    HINTERNET hSession = NULL, hUrl = NULL;
    BOOL success = FALSE;
    
    // Используем INTERNET_OPEN_TYPE_DIRECT вместо PRECONFIG,
    // чтобы запросы не блокировались системными прокси-серверами или VPN
    hSession = InternetOpenW(L"PingWWW_Agent", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (hSession) {
        DWORD timeout = 1500;
        InternetSetOptionW(hSession, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionW(hSession, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionW(hSession, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        
        // Флаги игнорирования кэша и принудительного перезапуска соединения
        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTP | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS;
        
        hUrl = InternetOpenUrlW(hSession, L"http://www.msftconnecttest.com/connecttest.txt", NULL, 0, flags, 0);
        if (hUrl) {
            DWORD statusCode = 0;
            DWORD length = sizeof(statusCode);
            if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &length, NULL)) {
                if (statusCode == 200) {
                    success = TRUE;
                }
            }
            InternetCloseHandle(hUrl);
        }
        InternetCloseHandle(hSession);
    }
    
    PostMessageW(hwnd, WM_APP_CHECK_DONE, (WPARAM)success, 0);
    return 0;
}

// Пересоздание шрифтов с учётом DPI текущего монитора.
// ВАЖНО: используем ANTIALIASED_QUALITY (grayscale-AA) вместо CLEARTYPE_QUALITY,
// потому что при отрисовке в 32-битную DIB-секцию с последующим UpdateLayeredWindow
// ClearType оставляет цветные субпиксельные каёмки — текст выглядит мыльным.
void RecreateFontsForDpi(HWND hwnd) {
    UINT dpi = 96;
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        PFN_GetDpiForWindow pGetDpi = (PFN_GetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
        if (pGetDpi && hwnd) {
            dpi = pGetDpi(hwnd);
        }
    }

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(LOGFONTW));
    lf.lfHeight  = -MulDiv(13, dpi, 96);
    lf.lfWeight  = FW_NORMAL;
    lf.lfQuality = ANTIALIASED_QUALITY;   // grayscale-AA, а не ClearType
    wcsncpy(lf.lfFaceName, L"Segoe UI", 32);

    if (g_hFontNormal) DeleteObject(g_hFontNormal);
    g_hFontNormal = CreateFontIndirectW(&lf);
}

// Функция форматирования вывода скоростей через наклонную черту
void FormatSpeedString(wchar_t* buffer, size_t bufferSize, const wchar_t* prefix, double currentKb, double avgKb) {
    wchar_t curStr[32];
    wchar_t avgStr[32];

    if (currentKb >= 1024.0) swprintf(curStr, 32, L"%.1f MB/s", currentKb / 1024.0);
    else swprintf(curStr, 32, L"%.1f KB/s", currentKb);

    if (avgKb >= 1024.0) swprintf(avgStr, 32, L"%.1f MB/s", avgKb / 1024.0);
    else swprintf(avgStr, 32, L"%.1f KB/s", avgKb);

    swprintf(buffer, bufferSize, L"%ls %ls / %ls", prefix, curStr, avgStr);
}

// Генерация круглого сглаженного индикатора
void UpdateMainLayeredWindow(void) {
    int size = g_sizes[g_sizeLevel];
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    int nSavedDC = SaveDC(hdcMem);
    
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(BITMAPINFO));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    void* pvBits = NULL;
    HBITMAP hbmpMem = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    SelectObject(hdcMem, hbmpMem);
    
    BYTE alphaValue = g_isTransparent ? 128 : 255;
    BYTE rTarget = g_isNetworkUp ? 46 : 231;
    BYTE gTarget = g_isNetworkUp ? 204 : 76;
    BYTE bTarget = g_isNetworkUp ? 113 : 60;
    
    double radius = size / 2.0;
    double cx = radius, cy = radius;
    
    DWORD* pixels = (DWORD*)pvBits;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double dx = (x + 0.5) - cx;
            double dy = (y + 0.5) - cy;
            double dist = sqrt(dx * dx + dy * dy);
            
            BYTE a = 0;
            if (dist <= radius - 0.5) a = alphaValue;
            else if (dist >= radius + 0.5) a = 0;
            else a = (BYTE)(alphaValue * (radius + 0.5 - dist));
            
            if (a > 0) {
                pixels[y * size + x] = (a << 24) | (((rTarget * a) / 255) << 16) | (((gTarget * a) / 255) << 8) | ((bTarget * a) / 255);
            } else pixels[y * size + x] = 0;
        }
    }
    
    POINT ptSrc = { 0, 0 }, ptDst = { g_posX, g_posY };
    SIZE sizeWindow = { size, size };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwndMain, hdcScreen, &ptDst, &sizeWindow, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    
    RestoreDC(hdcMem, nSavedDC);
    DeleteObject(hbmpMem);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// Отрисовка информационного поп-апа (DPI-зависимый layout, авто-ширина)
void UpdatePopupLayeredWindow(void) {
    if (!g_hwndPopup) return;

    // --- Определяем DPI текущего монитора ---
    UINT dpi = 96;
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        PFN_GetDpiForWindow pGetDpi = (PFN_GetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
        if (pGetDpi && g_hwndMain) dpi = pGetDpi(g_hwndMain);
    }

    // --- Масштабируемые размеры (в единицах 96 DPI) ---
    const int H96    = 178;   // высота окна
    const int PADX96 = 12;    // отступ слева/справа
    const int PADY96 = 12;    // отступ сверху/снизу
    const int LINE96 = 21;    // межстрочный шаг
    const int MINW96 = 260;   // минимальная ширина окна

    int h     = MulDiv(H96,    dpi, 96);
    int padX  = MulDiv(PADX96, dpi, 96);
    int padY  = MulDiv(PADY96, dpi, 96);
    int lineH = MulDiv(LINE96, dpi, 96);

    // --- Измеряем реальную ширину самой длинной строки ---
    HDC hdcMeasure = GetDC(NULL);
    SelectObject(hdcMeasure, g_hFontNormal);

    SIZE szRepo  = {0};
    SIZE szSpeed = {0};
    GetTextExtentPoint32W(hdcMeasure, L"https://github.com/IgerOK/ping-www", 36, &szRepo);
    GetTextExtentPoint32W(hdcMeasure, L"Speed In: 000.0 KB/s / 000.0 KB/s", 36, &szSpeed);
    ReleaseDC(NULL, hdcMeasure);

    int textW = (szRepo.cx > szSpeed.cx) ? szRepo.cx : szSpeed.cx;
    int w = textW + padX * 2 + MulDiv(8, dpi, 96);
    int minW = MulDiv(MINW96, dpi, 96);
    if (w < minW) w = minW;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    int nSavedDC = SaveDC(hdcMem);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(BITMAPINFO));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = NULL;
    HBITMAP hbmpMem = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    SelectObject(hdcMem, hbmpMem);

    // --- Фон со скруглёнными углами ---
    int cornerRadius = MulDiv(8, dpi, 96);
    DWORD* pixels = (DWORD*)pvBits;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            BOOL isInside = TRUE;
            if (x < cornerRadius && y < cornerRadius &&
                (x-cornerRadius)*(x-cornerRadius)+(y-cornerRadius)*(y-cornerRadius) > cornerRadius*cornerRadius) isInside = FALSE;
            if (x >= w-cornerRadius && y < cornerRadius &&
                (x-(w-cornerRadius))*(x-(w-cornerRadius))+(y-cornerRadius)*(y-cornerRadius) > cornerRadius*cornerRadius) isInside = FALSE;
            if (x < cornerRadius && y >= h-cornerRadius &&
                (x-cornerRadius)*(x-cornerRadius)+(y-(h-cornerRadius))*(y-(h-cornerRadius)) > cornerRadius*cornerRadius) isInside = FALSE;
            if (x >= w-cornerRadius && y >= h-cornerRadius &&
                (x-(w-cornerRadius))*(x-(w-cornerRadius))+(y-(h-cornerRadius))*(y-(h-cornerRadius)) > cornerRadius*cornerRadius) isInside = FALSE;

            if (isInside) {
                pixels[y * w + x] = (255u << 24); // чёрный, полностью непрозрачный
            } else {
                pixels[y * w + x] = 0;
            }
        }
    }

    // --- Статистика ---
    ULONGLONG currentIn = 0, currentOut = 0;
    GetTotalNetworkBytes(&currentIn, &currentOut);

    double mbIn  = (double)(currentIn  - g_initialInBytes)  / 1048576.0;
    double mbOut = (double)(currentOut - g_initialOutBytes) / 1048576.0;

    double avgSpeedInKb = 0.0;
    if (g_activeInSeconds > 0)
        avgSpeedInKb  = ((double)(currentIn  - g_initialInBytes)  / 1024.0) / (double)g_activeInSeconds;
    double avgSpeedOutKb = 0.0;
    if (g_activeOutSeconds > 0)
        avgSpeedOutKb = ((double)(currentOut - g_initialOutBytes) / 1024.0) / (double)g_activeOutSeconds;

    wchar_t lineRepo[]    = L"https://github.com/IgerOK/ping-www";
    wchar_t lineUptime[64];
    wchar_t lineTraffic[64];
    wchar_t lineSpeedIn[128];
    wchar_t lineSpeedOut[128];
    wchar_t lineHelp1[]   = L"2xLMB: Transp | RMB: Size";
    wchar_t lineHelp2[]   = L"LMB + Esc: Exit";

    DWORD elapsed = GetTickCount() - g_startTick;
    swprintf(lineUptime, 64, L"Uptime: %02u:%02u:%02u",
             elapsed / 3600000, (elapsed / 60000) % 60, (elapsed / 1000) % 60);
    swprintf(lineTraffic, 64, L"Traffic (In/Out): %.1f / %.1f MB", mbIn, mbOut);

    FormatSpeedString(lineSpeedIn,  128, L"Speed In: ",  g_currentSpeedInKb,  avgSpeedInKb);
    FormatSpeedString(lineSpeedOut, 128, L"Speed Out:",  g_currentSpeedOutKb, avgSpeedOutKb);

    SetBkMode(hdcMem, TRANSPARENT);
    SelectObject(hdcMem, g_hFontNormal);

    RECT rcText;
    rcText.left = padX;

    // --- Основной текст: сверху вниз ---
    SetTextColor(hdcMem, RGB(255, 255, 0));
    int top = padY;
    rcText.top = top; DrawTextW(hdcMem, lineRepo,     -1, &rcText, DT_NOCLIP); top += lineH;
    rcText.top = top; DrawTextW(hdcMem, lineUptime,   -1, &rcText, DT_NOCLIP); top += lineH;
    rcText.top = top; DrawTextW(hdcMem, lineTraffic,  -1, &rcText, DT_NOCLIP); top += lineH;
    rcText.top = top; DrawTextW(hdcMem, lineSpeedIn,  -1, &rcText, DT_NOCLIP); top += lineH;
    rcText.top = top; DrawTextW(hdcMem, lineSpeedOut, -1, &rcText, DT_NOCLIP);

    // --- Подсказки: снизу вверх, чтобы всегда прижимались к низу окна ---
    SetTextColor(hdcMem, RGB(200, 180, 0));
    int helpY2 = h - padY - lineH;
    int helpY1 = helpY2 - lineH;

    rcText.top = helpY1; DrawTextW(hdcMem, lineHelp1, -1, &rcText, DT_NOCLIP);
    rcText.top = helpY2; DrawTextW(hdcMem, lineHelp2, -1, &rcText, DT_NOCLIP);

    // --- Корректируем альфа-канал для букв ---
    for (int i = 0; i < w * h; i++) {
        DWORD p = pixels[i];
        if ((p >> 24) == 0 && (p & 0x00FFFFFF) != 0) {
            pixels[i] |= (255u << 24);
        }
    }

    // --- Позиционирование относительно индикатора ---
    int mainSize = g_sizes[g_sizeLevel];
    int margin   = MulDiv(8, dpi, 96);
    int popX = g_posX + mainSize + margin;
    int popY = g_posY + (mainSize - h) / 2;

    HMONITOR hMonitor = MonitorFromWindow(g_hwndMain, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMonitor, &mi)) {
        if (popX + w > mi.rcWork.right)  popX = g_posX - w - margin;
        if (popY < mi.rcWork.top)        popY = mi.rcWork.top;
        if (popY + h > mi.rcWork.bottom) popY = mi.rcWork.bottom - h;
    }

    POINT ptSrc = { 0, 0 }, ptDst = { popX, popY };
    SIZE  sizeWindow = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwndPopup, hdcScreen, &ptDst, &sizeWindow,
                        hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    RestoreDC(hdcMem, nSavedDC);
    DeleteObject(hbmpMem);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// Обработчик системных сообщений для поп-ап окна
LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Главный обработчик событий интерфейса (индикатор)
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_startTick = GetTickCount();
            
            // Создаём шрифт с учётом DPI текущего монитора
            RecreateFontsForDpi(hwnd);
            
            GetTotalNetworkBytes(&g_initialInBytes, &g_initialOutBytes);
            g_lastInBytes = g_initialInBytes;
            g_lastOutBytes = g_initialOutBytes;
            
            g_hwndPopup = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                POPUP_CLASS_NAME, NULL, WS_POPUP, 0, 0, 1, 1, hwnd, NULL, GetModuleHandle(NULL), NULL);
                
            SetTimer(hwnd, ID_TIMER_CHECK, 10, NULL);
            SetTimer(hwnd, ID_TIMER_SPEED_TRACK, 1000, NULL);
            return 0;
        }
        
        // Реакция на смену DPI (перетаскивание между мониторами с разным масштабом)
        case WM_DPICHANGED: {
            RecreateFontsForDpi(hwnd);
            if (g_isDragging) UpdatePopupLayeredWindow();
            return 0;
        }
        
        case WM_TIMER:
            if (wp == ID_TIMER_CHECK) {
                KillTimer(hwnd, ID_TIMER_CHECK);
                if (!g_isChecking) {
                    g_isChecking = TRUE;
                    CreateThread(NULL, 0, NetworkCheckThread, hwnd, 0, NULL);
                }
            } else if (wp == ID_TIMER_POPUP) {
                if (g_isDragging) UpdatePopupLayeredWindow();
            } else if (wp == ID_TIMER_SPEED_TRACK) {
                ULONGLONG currentIn = 0, currentOut = 0;
                GetTotalNetworkBytes(&currentIn, &currentOut);
                
                ULONGLONG diffIn = currentIn - g_lastInBytes;
                ULONGLONG diffOut = currentOut - g_lastOutBytes;
                
                g_currentSpeedInKb = (double)diffIn / 1024.0;
                g_currentSpeedOutKb = (double)diffOut / 1024.0;
                
                if (diffIn > 0) g_activeInSeconds++;
                if (diffOut > 0) g_activeOutSeconds++;
                
                g_lastInBytes = currentIn;
                g_lastOutBytes = currentOut;
            }
            return 0;
            
        case WM_APP_CHECK_DONE: {
            g_isNetworkUp = (BOOL)wp;
            g_isChecking = FALSE;
            UpdateMainLayeredWindow();
            SetTimer(hwnd, ID_TIMER_CHECK, g_isNetworkUp ? 5000 : 1000, NULL);
            return 0;
        }
        
        case WM_LBUTTONDOWN: {
            g_isDragging = TRUE;
            SetCapture(hwnd);
            GetCursorPos(&g_dragStartMouse);
            g_dragStartWindow.x = g_posX; g_dragStartWindow.y = g_posY;
            
            UpdatePopupLayeredWindow();
            ShowWindow(g_hwndPopup, SW_SHOWNOACTIVATE);
            SetTimer(hwnd, ID_TIMER_POPUP, 100, NULL);
            return 0;
        }
        
        case WM_MOUSEMOVE: {
            if (g_isDragging) {
                POINT pt; GetCursorPos(&pt);
                g_posX = g_dragStartWindow.x + (pt.x - g_dragStartMouse.x);
                g_posY = g_dragStartWindow.y + (pt.y - g_dragStartMouse.y);
                ClampPositionToMonitor(&g_posX, &g_posY, g_sizes[g_sizeLevel]);
                UpdateMainLayeredWindow();
                UpdatePopupLayeredWindow();
            }
            return 0;
        }
        
        case WM_LBUTTONUP: {
            if (g_isDragging) {
                g_isDragging = FALSE;
                ReleaseCapture();
                KillTimer(hwnd, ID_TIMER_POPUP);
                ShowWindow(g_hwndPopup, SW_HIDE);
            }
            return 0;
        }
        
        case WM_LBUTTONDBLCLK:
            g_isTransparent = !g_isTransparent;
            UpdateMainLayeredWindow();
            return 0;
            
        case WM_RBUTTONDOWN: {
            int oldSize = g_sizes[g_sizeLevel];
            g_sizeLevel = (g_sizeLevel + 1) % 3;
            int newSize = g_sizes[g_sizeLevel];
            g_posX = g_posX + (oldSize - newSize) / 2;
            g_posY = g_posY + (oldSize - newSize) / 2;
            ClampPositionToMonitor(&g_posX, &g_posY, newSize);
            UpdateMainLayeredWindow();
            if (g_isDragging) UpdatePopupLayeredWindow();
            return 0;
        }
        
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && g_isDragging) SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
            
        case WM_CLOSE:
            SaveSettings();
            DestroyWindow(hwnd);
            return 0;
            
        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER_CHECK);
            KillTimer(hwnd, ID_TIMER_POPUP);
            KillTimer(hwnd, ID_TIMER_SPEED_TRACK);
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Главная точка входа WinMain
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    // Включаем per-monitor DPI-awareness ДО создания окон.
    // Без этого Windows растягивает всё окно целиком, что мылит текст.
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
        PFN_SetCtx pSetCtx = (PFN_SetCtx)GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
        if (pSetCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4
            pSetCtx((HANDLE)-4);
        } else {
            // Fallback для Windows 7/8
            SetProcessDPIAware();
        }
    }
    
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"PingWWW_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Программа PingWWW уже запущена!", L"Предупреждение", MB_OK | MB_ICONWARNING);
        return 0;
    }
    
    LoadSettings();
    
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(WNDCLASSEXW));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.style = CS_DBLCLKS; 
    RegisterClassExW(&wc);
    
    wc.lpfnWndProc = PopupWndProc;
    wc.lpszClassName = POPUP_CLASS_NAME;
    wc.style = 0;
    RegisterClassExW(&wc);
    
    int currentSize = g_sizes[g_sizeLevel];
    g_hwndMain = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        WINDOW_CLASS_NAME, L"PingWWW", WS_POPUP,
        g_posX, g_posY, currentSize, currentSize, NULL, NULL, hInst, NULL);
        
    if (!g_hwndMain) return 0;
    
    ShowWindow(g_hwndMain, nShow);
    UpdateWindow(g_hwndMain);
    UpdateMainLayeredWindow();
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}
