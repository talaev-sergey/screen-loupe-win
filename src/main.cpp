// Screen Loupe — экранная лупа для проектора (Windows 11).
//
// Затемняет весь экран, кроме круга у курсора. Колесо мыши увеличивает картинку
// в круге. Включение/выключение — Win+Z, выключение — Esc.
//
// Устройство:
// - затемнение — неподвижное полупрозрачное окно на весь виртуальный экран;
//   оно не перерисовывается, пока лупа включена;
// - круг — маленькое окно поверх затемнения с элементом Magnification API.
//   Этот элемент показывает экран без наших окон (фильтр-лист), поэтому при
//   увеличении 1× круг выглядит как дырка в затемнении, а при >1× — как лупа;
// - клавиатура и колесо — low-level хуки в отдельном потоке, чтобы занятый
//   отрисовкой главный поток не тормозил ввод во всей системе.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <magnification.h>

#include <atomic>
#include <climits>

// --- настройки ---
constexpr int RADIUS = 130;                 // радиус лупы, px
constexpr BYTE DIM_ALPHA = 150;             // затемнение снаружи (0..255)
constexpr UINT FRAME_MS = 10;               // период кадра; Windows округлит до ~15.6 мс (~64 к/с)
constexpr int BORDER = 2;                   // толщина рамки круга, px
constexpr COLORREF BORDER_COLOR = RGB(200, 200, 200);
constexpr double ZOOM_STEP = 0.5;           // прирост увеличения на щелчок колеса
constexpr double ZOOM_MAX = 8.0;

enum : UINT { WM_APP_TOGGLE = WM_APP + 1, WM_APP_HIDE, WM_APP_WHEEL, WM_APP_MOUSEHOOK };

static HWND g_dim, g_lens, g_mag;
static DWORD g_inputThread;
static std::atomic<bool> g_visible;
static double g_zoom;
static POINT g_last;

[[noreturn]] static void Fail(const wchar_t* what) {
    MessageBoxW(nullptr, what, L"Screen Loupe не запустилась", MB_ICONERROR);
    ExitProcess(1);
}

// --- поток ввода ---

// Win, отпущенный без другой клавиши, открывает «Пуск». Z мы съели, поэтому
// подсовываем ничейную клавишу 0xE8: для Windows Win становится частью сочетания.
static void MaskWinKey() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    // Флаги удержания: автоповтор не должен дёргать лупу, а отпускание съеденной
    // клавиши не должно долетать до приложения.
    static bool zHeld, escHeld;
    if (code == HC_ACTION) {
        DWORD vk = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp)->vkCode;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (vk == 'Z') {
            bool win = (GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000;
            if (down && (win || zHeld)) {
                if (!zHeld) {
                    zHeld = true;
                    MaskWinKey();
                    PostMessageW(g_dim, WM_APP_TOGGLE, 0, 0);
                }
                return 1;
            }
            if (!down && zHeld) {
                zHeld = false;
                return 1;
            }
        }
        if (vk == VK_ESCAPE) {
            if (down && (g_visible || escHeld)) {
                if (!escHeld) {
                    escHeld = true;
                    PostMessageW(g_dim, WM_APP_HIDE, 0, 0);
                }
                return 1;
            }
            if (!down && escHeld) {
                escHeld = false;
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Ставится только на время показа лупы: колесо крутит увеличение, а не страницу.
static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL) {
        short delta = HIWORD(reinterpret_cast<MSLLHOOKSTRUCT*>(lp)->mouseData);
        PostMessageW(g_dim, WM_APP_WHEEL, 0, delta);
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI InputThread(void*) {
    if (!SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0))
        Fail(L"Не удалось поставить хук клавиатуры.");
    HHOOK mouse = nullptr;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message != WM_APP_MOUSEHOOK)
            continue;
        if (msg.wParam && !mouse) {
            mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
        } else if (!msg.wParam && mouse) {
            UnhookWindowsHookEx(mouse);
            mouse = nullptr;
        }
    }
    return 0;
}

// --- лупа ---

static void SetZoom(double z) {
    g_zoom = z < 1.0 ? 1.0 : z > ZOOM_MAX ? ZOOM_MAX : z;
    MAGTRANSFORM t = {};
    t.v[0][0] = t.v[1][1] = static_cast<float>(g_zoom);
    t.v[2][2] = 1.0f;
    MagSetWindowTransform(g_mag, &t);
}

static void Tick() {
    POINT p;
    GetCursorPos(&p);
    if (p.x != g_last.x || p.y != g_last.y) {
        g_last = p;
        SetWindowPos(g_lens, nullptr, p.x - RADIUS, p.y - RADIUS, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Источник — квадрат с центром в курсоре, в zoom раз меньше круга. Центр круга
    // тоже в курсоре, поэтому клик попадает ровно туда, куда показывает увеличение.
    int s = static_cast<int>((2 * (RADIUS - BORDER)) / g_zoom);
    RECT src = {p.x - s / 2, p.y - s / 2, p.x - s / 2 + s, p.y - s / 2 + s};
    MagSetWindowSource(g_mag, src);
    // Перерисовываем и при неподвижном курсоре: под лупой может идти видео.
    InvalidateRect(g_mag, nullptr, FALSE);
}

static void Show() {
    SetWindowPos(g_dim, HWND_TOPMOST,
                 GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_last = {LONG_MIN, LONG_MIN};
    Tick();
    ShowWindow(g_lens, SW_SHOWNOACTIVATE);
    SetTimer(g_dim, 1, FRAME_MS, nullptr);
    g_visible = true;
    PostThreadMessageW(g_inputThread, WM_APP_MOUSEHOOK, TRUE, 0);
}

static void Hide() {
    g_visible = false;
    PostThreadMessageW(g_inputThread, WM_APP_MOUSEHOOK, FALSE, 0);
    KillTimer(g_dim, 1);
    ShowWindow(g_lens, SW_HIDE);
    ShowWindow(g_dim, SW_HIDE);
}

static LRESULT CALLBACK DimProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_TOGGLE:
        g_visible ? Hide() : Show();
        return 0;
    case WM_APP_HIDE:
        if (g_visible)
            Hide();
        return 0;
    case WM_APP_WHEEL:
        SetZoom(g_zoom + ZOOM_STEP * static_cast<int>(lp) / WHEEL_DELTA);
        return 0;
    case WM_TIMER:
        Tick();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // Один экземпляр: автозапуск и ручной запуск не должны плодить копии.
    CreateMutexW(nullptr, TRUE, L"Local\\ScreenLoupe");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    // Все координаты — в физических пикселях, иначе на мониторах с масштабом
    // 125–150 % лупа съезжает от курсора.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!MagInitialize())
        Fail(L"Не удалось инициализировать Magnification API.");

    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = DimProc;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"ScreenLoupeDim";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hbrBackground = CreateSolidBrush(BORDER_COLOR);  // фон круга виден как рамка
    wc.lpszClassName = L"ScreenLoupeLens";
    RegisterClassW(&wc);

    // Поверх всех, сквозь клики, без фокуса, без кнопки на панели задач.
    const DWORD ex = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                     WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    g_dim = CreateWindowExW(ex, L"ScreenLoupeDim", L"Screen Loupe", WS_POPUP,
                            0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_dim, 0, DIM_ALPHA, LWA_ALPHA);

    // Круг принадлежит затемнению, поэтому всегда лежит над ним.
    const int d = 2 * RADIUS, m = 2 * (RADIUS - BORDER);
    g_lens = CreateWindowExW(ex, L"ScreenLoupeLens", L"", WS_POPUP,
                             0, 0, d, d, g_dim, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_lens, 0, 255, LWA_ALPHA);
    SetWindowRgn(g_lens, CreateEllipticRgn(0, 0, d + 1, d + 1), FALSE);

    g_mag = CreateWindowW(WC_MAGNIFIER, L"", WS_CHILD | WS_VISIBLE,
                          BORDER, BORDER, m, m, g_lens, nullptr, inst, nullptr);
    if (!g_mag)
        Fail(L"Не удалось создать окно лупы.");
    SetWindowRgn(g_mag, CreateEllipticRgn(0, 0, m + 1, m + 1), FALSE);
    HWND own[] = {g_dim, g_lens};
    MagSetWindowFilterList(g_mag, MW_FILTERMODE_EXCLUDE, 2, own);  // лупа не видит себя
    SetZoom(1.0);

    CreateThread(nullptr, 0, InputThread, nullptr, 0, &g_inputThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}
