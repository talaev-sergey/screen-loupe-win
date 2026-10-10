// Screen Loupe — экранная лупа и перо для проектора (Windows 11).
//
// Лупа: затемняет весь экран, кроме круга у курсора. Колесо мыши увеличивает
// картинку в круге, Ctrl+колесо меняет размер круга. Включение/выключение —
// Win+Z, выключение — Esc.
// Перо: Win+Shift+Z — рисовать поверх экрана мягкими цветами. Shift — прямая,
// Ctrl — стрелка, 1–7 — цвет, колесо — толщина, Ctrl+Z — отменить, Delete —
// стереть всё, Esc — выйти.
//
// Устройство:
// - затемнение — неподвижное полупрозрачное окно на весь виртуальный экран;
//   оно не перерисовывается, пока лупа включена;
// - круг — маленькое окно поверх затемнения с элементом Magnification API.
//   Этот элемент показывает экран без наших окон, поэтому при
//   увеличении 1× круг выглядит как дырка в затемнении, а при >1× — как лупа;
// - обводка — отдельное окно над кругом с попиксельной прозрачностью: край
//   региона окна всегда ступенчатый, а у обводки края сглажены;
// - перо — окно с попиксельной прозрачностью на весь экран, рисунок — GDI+;
// - клавиатура и колесо — low-level хуки в отдельном потоке, чтобы занятый
//   отрисовкой главный поток не тормозил ввод во всей системе;
// - обновление — фоновый поток: при старте сверяет версию с последним релизом
//   на GitHub и молча ставит новую.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <magnification.h>
#include <objidl.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

// GDI+ ждёт макросы min/max, которые выключены NOMINMAX.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

// --- настройки ---
constexpr int RADIUS = 130;                 // радиус лупы при запуске, px
constexpr int RADIUS_MIN = 60, RADIUS_MAX = 400;
constexpr int RADIUS_STEP = 20;             // шаг Ctrl+колеса, px
constexpr BYTE DIM_ALPHA = 150;             // затемнение снаружи (0..255)
constexpr UINT FRAME_MS = 10;               // таймер кадра; сам кадр ждёт DWM, то есть частоту экрана
constexpr int BORDER = 2;                   // толщина рамки круга, px
constexpr COLORREF BORDER_COLOR = RGB(0xFB, 0x5E, 0xDE);  // #FB5EDE
constexpr double ZOOM_STEP = 0.5;           // прирост увеличения на щелчок колеса
constexpr double ZOOM_MAX = 8.0;
// Цвета пера, клавиши 1–7. Мягкие, но читаются и на белом, и на тёмном.
constexpr Gdiplus::ARGB PEN_COLORS[] = {
    0xFFF472B6,  // 1 розовый
    0xFFF87171,  // 2 красный
    0xFFFB923C,  // 3 оранжевый
    0xFFFACC15,  // 4 жёлтый
    0xFF4ADE80,  // 5 зелёный
    0xFF60A5FA,  // 6 голубой
    0xFFA78BFA,  // 7 сиреневый
};
constexpr float PEN_WIDTH = 6, PEN_WIDTH_MIN = 2, PEN_WIDTH_MAX = 40;
constexpr float PEN_WIDTH_STEP = 2;         // шаг колеса, px
constexpr DWORD UPDATE_DELAY_MS = 60 * 1000;        // первая проверка обновления после старта
constexpr DWORD UPDATE_RETRY_MS = 15 * 60 * 1000;   // повтор, если сеть недоступна
constexpr wchar_t RELEASE_URL[] =
    L"https://github.com/talaev-sergey/screen-loupe-win/releases/latest/download/";

enum : UINT {
    WM_APP_TOGGLE = WM_APP + 1,  // Win+Z
    WM_APP_PEN,                  // Win+Shift+Z
    WM_APP_HIDE,                 // Esc
    WM_APP_WHEEL,                // wParam — зажат ли Ctrl, lParam — поворот колеса
    WM_APP_PENKEY,               // wParam — клавиша пера
    WM_APP_MOUSEHOOK,
};
enum : int { MODE_OFF, MODE_LOUPE, MODE_PEN };

static HWND g_dim, g_lens, g_mag, g_ring, g_pen;
static DWORD g_inputThread;
static std::atomic<int> g_mode;
static double g_zoom;
static int g_radius;
static POINT g_last;

[[noreturn]] static void Fail(const wchar_t* what) {
    MessageBoxW(nullptr, what, L"Screen Loupe не запустилась", MB_ICONERROR);
    ExitProcess(1);
}

static bool KeyDown(int vk) {
    return GetAsyncKeyState(vk) & 0x8000;
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
    // Съеденная клавиша: её автоповтор не должен дёргать лупу, а ни повтор, ни
    // отпускание не должны долетать до приложения.
    static DWORD held;
    if (code == HC_ACTION) {
        DWORD vk = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp)->vkCode;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (vk == held) {
            if (!down)
                held = 0;
            return 1;
        }
        if (!down)
            return CallNextHookEx(nullptr, code, wp, lp);
        UINT msg = 0;
        if (vk == 'Z' && (KeyDown(VK_LWIN) || KeyDown(VK_RWIN))) {
            MaskWinKey();
            msg = KeyDown(VK_SHIFT) ? WM_APP_PEN : WM_APP_TOGGLE;
        } else if (vk == VK_ESCAPE && g_mode != MODE_OFF) {
            msg = WM_APP_HIDE;
        } else if (g_mode == MODE_PEN && ((vk >= '1' && vk < '1' + std::size(PEN_COLORS)) ||
                                          vk == VK_DELETE || (vk == 'Z' && KeyDown(VK_CONTROL)))) {
            msg = WM_APP_PENKEY;
        }
        if (msg) {
            held = vk;
            PostMessageW(g_dim, msg, vk, 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Ставится только на время лупы или пера: колесо меняет увеличение, размер
// круга или толщину линии, а не крутит страницу.
static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL) {
        short delta = HIWORD(reinterpret_cast<MSLLHOOKSTRUCT*>(lp)->mouseData);
        PostMessageW(g_dim, WM_APP_WHEEL, KeyDown(VK_CONTROL), delta);
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

// Когда после Win+Z отпускают Win, Explorer поднимает панель задач над всеми
// topmost-окнами. Видимое окно над нашим верхним окном top — поднимаем наверх
// raise (окна, которыми оно владеет, поднимаются следом). Невидимые служебные
// окна Windows висят выше всегда, их не считаем.
static void KeepOnTop(HWND top, HWND raise) {
    for (HWND h = GetWindow(top, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV)) {
        if (IsWindowVisible(h)) {
            SetWindowPos(raise, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        }
    }
}

// --- лупа ---

static void SetZoom(double z) {
    g_zoom = z < 1.0 ? 1.0 : z > ZOOM_MAX ? ZOOM_MAX : z;
    MAGTRANSFORM t = {};
    t.v[0][0] = t.v[1][1] = static_cast<float>(g_zoom);
    t.v[2][2] = 1.0f;
    MagSetWindowTransform(g_mag, &t);
}

// Круг из пикселей, чей центр не дальше r от центра окна. Обводка считает
// прозрачность по тем же центрам пикселей, поэтому ступенчатый край круга
// всегда лежит под её непрозрачной серединой.
static HRGN CircleRgn(double r) {
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    for (int y = 0; y < 2 * g_radius; y++) {
        double dy = y + 0.5 - g_radius;
        if (dy * dy > r * r)
            continue;
        double dx = std::sqrt(r * r - dy * dy);
        HRGN row = CreateRectRgn(static_cast<int>(std::ceil(g_radius - 0.5 - dx)), y,
                                 static_cast<int>(std::floor(g_radius - 0.5 + dx)) + 1, y + 1);
        CombineRgn(rgn, rgn, row, RGN_OR);
        DeleteObject(row);
    }
    return rgn;
}

// Рисует обводку: кольцо толщиной BORDER, края сглажены на пиксель.
static void PaintRing() {
    const int d = 2 * g_radius;
    const double outer = g_radius - 1, inner = outer - BORDER;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = d;
    bi.bmiHeader.biHeight = -d;  // строки сверху вниз
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto px = static_cast<DWORD*>(bits);
    for (int y = 0; y < d; y++) {
        for (int x = 0; x < d; x++) {
            double r = std::hypot(x + 0.5 - g_radius, y + 0.5 - g_radius);
            double a = std::clamp(std::min(outer - r, r - inner) + 0.5, 0.0, 1.0);
            // UpdateLayeredWindow ждёт цвет, уже умноженный на прозрачность.
            // COLORREF — 0x00BBGGRR, пиксель — 0xAARRGGBB.
            auto ch = [a](DWORD c) { return static_cast<DWORD>((c & 0xFF) * a + 0.5); };
            px[y * d + x] = ch(255) << 24 | ch(BORDER_COLOR) << 16 |
                            ch(BORDER_COLOR >> 8) << 8 | ch(BORDER_COLOR >> 16);
        }
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    SIZE size = {d, d};
    POINT origin = {};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_ring, nullptr, nullptr, &size, dc, &origin, 0, &blend, ULW_ALPHA);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// Размер круга: окна, их вырез и обводка. Круг сразу ставим по курсору,
// иначе на кадр он съехал бы вправо-вниз.
static void SetRadius(int r) {
    g_radius = std::clamp(r, RADIUS_MIN, RADIUS_MAX);
    const int d = 2 * g_radius;
    const double edge = g_radius - 1 - BORDER / 2.0;
    GetCursorPos(&g_last);
    HDWP dwp = BeginDeferWindowPos(2);
    for (HWND h : {g_lens, g_ring})
        dwp = DeferWindowPos(dwp, h, nullptr, g_last.x - g_radius, g_last.y - g_radius, d, d,
                             SWP_NOZORDER | SWP_NOACTIVATE);
    EndDeferWindowPos(dwp);
    SetWindowPos(g_mag, nullptr, 0, 0, d, d, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowRgn(g_lens, CircleRgn(edge), TRUE);
    SetWindowRgn(g_mag, CircleRgn(edge), TRUE);
    PaintRing();
}

static void Tick() {
    // Кадр начинаем сразу после того, как DWM собрал предыдущий, а окно круга
    // двигаем только после перерисовки картинки: так оба изменения попадают в
    // один кадр. Иначе окно уезжает раньше картинки, и она дрожит при движении.
    DwmFlush();
    POINT p;
    GetCursorPos(&p);
    // Источник — квадрат с центром в курсоре, в zoom раз меньше круга. Центр круга
    // тоже в курсоре, поэтому клик попадает ровно туда, куда показывает увеличение.
    int s = static_cast<int>((2 * g_radius) / g_zoom);
    RECT src = {p.x - s / 2, p.y - s / 2, p.x - s / 2 + s, p.y - s / 2 + s};
    MagSetWindowSource(g_mag, src);
    // Перерисовываем и при неподвижном курсоре: под лупой может идти видео.
    InvalidateRect(g_mag, nullptr, FALSE);
    UpdateWindow(g_mag);
    if (p.x != g_last.x || p.y != g_last.y) {
        g_last = p;
        // Круг и обводку двигаем одной операцией, чтобы они не разъезжались.
        HDWP dwp = BeginDeferWindowPos(2);
        for (HWND h : {g_lens, g_ring})
            dwp = DeferWindowPos(dwp, h, nullptr, p.x - g_radius, p.y - g_radius, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        EndDeferWindowPos(dwp);
    }
    // Круг с обводкой принадлежат затемнению.
    KeepOnTop(g_ring, g_dim);
}

static void ShowLoupe() {
    SetWindowPos(g_dim, HWND_TOPMOST,
                 GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_last = {LONG_MIN, LONG_MIN};
    Tick();
    ShowWindow(g_lens, SW_SHOWNOACTIVATE);
    ShowWindow(g_ring, SW_SHOWNOACTIVATE);
    SetTimer(g_dim, 1, FRAME_MS, nullptr);
    g_mode = MODE_LOUPE;
    PostThreadMessageW(g_inputThread, WM_APP_MOUSEHOOK, TRUE, 0);
}

static void HideLoupe() {
    g_mode = MODE_OFF;
    PostThreadMessageW(g_inputThread, WM_APP_MOUSEHOOK, FALSE, 0);
    KillTimer(g_dim, 1);
    ShowWindow(g_ring, SW_HIDE);
    ShowWindow(g_lens, SW_HIDE);
    ShowWindow(g_dim, SW_HIDE);
}

// --- перо ---

// Пустой пиксель — почти прозрачный чёрный: полностью прозрачные пиксели
// слоистое окно пропускает к окнам под ним, и рисовать было бы не на чем.
constexpr DWORD PEN_CLEAR = 0x01000000;

enum Shape { FREEHAND, LINE, ARROW };
struct Stroke {
    Shape shape;
    Gdiplus::ARGB color;
    float width;
    std::vector<Gdiplus::PointF> pts;  // у прямой и стрелки — начало и конец
};

static std::vector<Stroke> g_strokes;  // последняя — та, что тянут, пока g_drawing
static bool g_drawing;
static size_t g_color;
static float g_width = PEN_WIDTH;
static HCURSOR g_cursor;
// Картинка на экране — DIB в g_penDC; g_base — она же без линии, которую тянут.
static int g_penX, g_penY, g_penW, g_penH;
static HDC g_penDC;
static HBITMAP g_penBmp;
static HGDIOBJ g_penOld;
static DWORD* g_view;
static std::vector<DWORD> g_base;
static RECT g_preview;  // где прямая или стрелка была в прошлом кадре

// Рисует линию с точки first: при рисовании от руки — только новый отрезок.
static void DrawStroke(DWORD* px, const Stroke& s, size_t first) {
    Gdiplus::Bitmap bmp(g_penW, g_penH, g_penW * 4, PixelFormat32bppPARGB,
                        reinterpret_cast<BYTE*>(px));
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Color color(s.color);
    if (s.pts.size() - first < 2) {  // просто щелчок — точка
        Gdiplus::SolidBrush brush(color);
        g.FillEllipse(&brush, s.pts[first].X - s.width / 2, s.pts[first].Y - s.width / 2,
                      s.width, s.width);
        return;
    }
    Gdiplus::Pen pen(color, s.width);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::AdjustableArrowCap arrow(3.0f, 3.0f, TRUE);
    if (s.shape == ARROW)
        pen.SetCustomEndCap(&arrow);
    g.DrawLines(&pen, &s.pts[first], static_cast<INT>(s.pts.size() - first));
}

static RECT StrokeRect(const Stroke& s, size_t first) {
    RECT r = {LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    for (size_t i = first; i < s.pts.size(); i++) {
        r.left = std::min(r.left, static_cast<LONG>(s.pts[i].X));
        r.top = std::min(r.top, static_cast<LONG>(s.pts[i].Y));
        r.right = std::max(r.right, static_cast<LONG>(s.pts[i].X) + 1);
        r.bottom = std::max(r.bottom, static_cast<LONG>(s.pts[i].Y) + 1);
    }
    int pad = static_cast<int>(s.width * 3) + 2;  // наконечник стрелки шире линии
    InflateRect(&r, pad, pad);
    return r;
}

static void PenCopy(DWORD* dst, const DWORD* src, RECT r) {
    RECT all = {0, 0, g_penW, g_penH};
    IntersectRect(&r, &r, &all);
    for (int y = r.top; y < r.bottom; y++)
        std::copy(src + y * g_penW + r.left, src + y * g_penW + r.right, dst + y * g_penW + r.left);
}

// Выводит картинку на экран; dirty — только этот прямоугольник.
static void PenPresent(const RECT* dirty) {
    RECT r = {0, 0, g_penW, g_penH};
    if (dirty)
        IntersectRect(&r, &r, dirty);
    POINT pos = {g_penX, g_penY}, origin = {};
    SIZE size = {g_penW, g_penH};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info = {sizeof(info), nullptr, &pos, &size, g_penDC, &origin,
                                    0, &blend, ULW_ALPHA, &r};
    UpdateLayeredWindowIndirect(g_pen, &info);
}

// Перерисовывает всё с нуля: после отмены и стирания.
static void PenRedraw() {
    std::fill(g_base.begin(), g_base.end(), PEN_CLEAR);
    for (const Stroke& s : g_strokes)
        DrawStroke(g_base.data(), s, 0);
    std::copy(g_base.begin(), g_base.end(), g_view);
    PenPresent(nullptr);
}

// Курсор пера — кружок текущего цвета и толщины: видно, чем будешь рисовать.
static void PenCursor() {
    const int d = static_cast<int>(std::max(g_width, 6.0f)) + 4;
    Gdiplus::Bitmap bmp(d, d, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush fill{Gdiplus::Color(PEN_COLORS[g_color])};
        Gdiplus::Pen edge(Gdiplus::Color(160, 0, 0, 0), 1);  // кант, чтобы жёлтый был виден на белом
        g.FillEllipse(&fill, 1.5f, 1.5f, d - 3.0f, d - 3.0f);
        g.DrawEllipse(&edge, 1.5f, 1.5f, d - 3.0f, d - 3.0f);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    ICONINFO ii = {};
    GetIconInfo(icon, &ii);
    ii.fIcon = FALSE;
    ii.xHotspot = ii.yHotspot = static_cast<DWORD>(d / 2);
    HCURSOR cursor = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    DestroyIcon(icon);
    if (g_cursor)
        DestroyCursor(g_cursor);
    g_cursor = cursor;
    SetCursor(g_cursor);
}

static void PenKey(WPARAM vk) {
    if (vk >= '1' && vk < '1' + std::size(PEN_COLORS)) {
        g_color = vk - '1';
        PenCursor();
        return;
    }
    if (g_drawing)  // линию, которую ещё тянут, не трогаем
        return;
    if (vk == VK_DELETE)
        g_strokes.clear();
    else if (!g_strokes.empty())  // Ctrl+Z
        g_strokes.pop_back();
    PenRedraw();
}

static void SetPenWidth(double w) {
    g_width = static_cast<float>(std::clamp<double>(w, PEN_WIDTH_MIN, PEN_WIDTH_MAX));
    PenCursor();
}

static void ShowPen() {
    g_penX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_penY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_penW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_penH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_penW;
    bi.bmiHeader.biHeight = -g_penH;  // строки сверху вниз
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    g_penBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_penBmp)
        return;
    g_view = static_cast<DWORD*>(bits);
    g_penDC = CreateCompatibleDC(nullptr);
    g_penOld = SelectObject(g_penDC, g_penBmp);
    g_base.assign(static_cast<size_t>(g_penW) * g_penH, PEN_CLEAR);
    PenRedraw();
    SetWindowPos(g_pen, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    PenCursor();
    SetTimer(g_dim, 1, FRAME_MS, nullptr);
    g_mode = MODE_PEN;
    PostThreadMessageW(g_inputThread, WM_APP_MOUSEHOOK, TRUE, 0);
}

// Выключает перо и стирает рисунок.
static void HidePen() {
    g_mode = MODE_OFF;
    PostThreadMessageW(g_inputThread, WM_APP_MOUSEHOOK, FALSE, 0);
    KillTimer(g_dim, 1);
    g_drawing = false;
    ReleaseCapture();
    ShowWindow(g_pen, SW_HIDE);
    g_strokes.clear();
    SelectObject(g_penDC, g_penOld);
    DeleteDC(g_penDC);
    DeleteObject(g_penBmp);
    std::vector<DWORD>().swap(g_base);
}

static LRESULT CALLBACK PenProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Gdiplus::PointF p(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(g_cursor);
        return TRUE;
    case WM_LBUTTONDOWN: {
        Shape shape = wp & MK_CONTROL ? ARROW : wp & MK_SHIFT ? LINE : FREEHAND;
        g_strokes.push_back({shape, PEN_COLORS[g_color], g_width, {p}});
        if (shape != FREEHAND)
            g_strokes.back().pts.push_back(p);
        g_drawing = true;
        g_preview = {};
        SetCapture(hwnd);
    }
        [[fallthrough]];
    case WM_MOUSEMOVE: {
        if (!g_drawing)
            return 0;
        Stroke& s = g_strokes.back();
        RECT r;
        if (msg == WM_MOUSEMOVE && s.shape == FREEHAND)
            s.pts.push_back(p);
        if (s.shape == FREEHAND) {
            size_t first = s.pts.size() < 2 ? 0 : s.pts.size() - 2;
            r = StrokeRect(s, first);
            DrawStroke(g_view, s, first);
        } else {
            // Прямую и стрелку тянут: стираем прошлое положение, рисуем новое.
            s.pts[1] = p;
            RECT now = StrokeRect(s, 0);
            UnionRect(&r, &g_preview, &now);
            PenCopy(g_view, g_base.data(), r);
            DrawStroke(g_view, s, 0);
            g_preview = now;
        }
        PenPresent(&r);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g_drawing) {
            g_drawing = false;
            ReleaseCapture();
            std::copy(g_view, g_view + g_base.size(), g_base.begin());
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- обновление ---

// Скачивает файл целиком в память. WinHTTP ничего не кэширует, поэтому
// version.txt всегда свежий; прокси берётся системный.
static bool Fetch(const wchar_t* name, std::string& out) {
    std::wstring url = std::wstring(RELEASE_URL) + name;
    wchar_t host[256], path[1024];
    URL_COMPONENTS u = {};
    u.dwStructSize = sizeof(u);
    u.lpszHostName = host;
    u.dwHostNameLength = ARRAYSIZE(host);
    u.lpszUrlPath = path;
    u.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u))
        return false;
    HINTERNET s = WinHttpOpen(L"ScreenLoupe/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, nullptr, nullptr,
                                         WINHTTP_FLAG_SECURE)
                    : nullptr;
    DWORD status = 0, len = sizeof(status);
    bool ok = r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr) &&
              WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  nullptr, &status, &len, nullptr) &&
              status == 200;
    out.clear();
    while (ok) {
        DWORD n = 0;
        ok = WinHttpQueryDataAvailable(r, &n);
        if (!ok || !n)
            break;
        size_t old = out.size();
        out.resize(old + n);
        ok = WinHttpReadData(r, &out[old], n, &n);
        out.resize(old + n);
    }
    for (HINTERNET h : {r, c, s})
        if (h)
            WinHttpCloseHandle(h);
    return ok;
}

// "1.2.3" → 1002003. Неразборчивое — 0, то есть старее любой версии.
static long ParseVersion(const char* s) {
    int a = 0, b = 0, c = 0;
    return sscanf_s(s, "%d.%d.%d", &a, &b, &c) == 3 ? a * 1000000L + b * 1000L + c : 0;
}

static std::string Sha256Hex(const std::string& data) {
    UCHAR h[32];
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
               reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
               static_cast<ULONG>(data.size()), h, sizeof(h));
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf_s(hex + 2 * i, 3, "%02x", h[i]);
    return hex;
}

// Молча ставит последний релиз, если он новее. Установщик сам останавливает
// эту копию и запускает новую. Без сети пробуем снова; не прошла проверка
// хеша — ждём следующего запуска.
static DWORD WINAPI UpdateThread(void*) {
    Sleep(UPDATE_DELAY_MS);  // после входа в Windows сеть поднимается не сразу
    std::string version, sums, setup;
    while (!Fetch(L"version.txt", version))
        Sleep(UPDATE_RETRY_MS);
    if (ParseVersion(version.c_str()) <= ParseVersion(APP_VERSION))
        return 0;
    if (!Fetch(L"checksums.txt", sums) || !Fetch(L"ScreenLoupe-Setup.exe", setup) ||
        sums.find(Sha256Hex(setup) + "  ScreenLoupe-Setup.exe") == std::string::npos)
        return 0;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring exe = std::wstring(tmp) + L"ScreenLoupe-Setup.exe";
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    DWORD written = 0;
    bool ok = WriteFile(f, setup.data(), static_cast<DWORD>(setup.size()), &written, nullptr) &&
              written == setup.size();
    CloseHandle(f);
    if (!ok)
        return 0;

    while (g_mode != MODE_OFF)  // не обрывать лупу и перо посреди урока
        Sleep(1000);
    std::wstring cmd = L"\"" + exe + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=\"" +
                       tmp + L"ScreenLoupe-update.log\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

static LRESULT CALLBACK DimProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_TOGGLE:
        if (g_mode == MODE_PEN)
            HidePen();
        g_mode == MODE_LOUPE ? HideLoupe() : ShowLoupe();
        return 0;
    case WM_APP_PEN:
        if (g_mode == MODE_LOUPE)
            HideLoupe();
        g_mode == MODE_PEN ? HidePen() : ShowPen();
        return 0;
    case WM_APP_HIDE:
        if (g_mode == MODE_LOUPE)
            HideLoupe();
        else if (g_mode == MODE_PEN)
            HidePen();
        return 0;
    case WM_APP_WHEEL: {
        double steps = static_cast<int>(lp) / static_cast<double>(WHEEL_DELTA);
        if (g_mode == MODE_PEN)
            SetPenWidth(g_width + steps * PEN_WIDTH_STEP);
        else if (wp)
            SetRadius(g_radius + static_cast<int>(steps * RADIUS_STEP));
        else
            SetZoom(g_zoom + steps * ZOOM_STEP);
        return 0;
    }
    case WM_APP_PENKEY:
        if (g_mode == MODE_PEN)
            PenKey(wp);
        return 0;
    case WM_TIMER:
        if (g_mode == MODE_LOUPE)
            Tick();
        else if (g_mode == MODE_PEN)
            KeepOnTop(g_pen, g_pen);
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
    Gdiplus::GdiplusStartupInput gdiplus;
    ULONG_PTR gdiplusToken;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplus, nullptr) != Gdiplus::Ok)
        Fail(L"Не удалось инициализировать GDI+.");

    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = DimProc;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"ScreenLoupeDim";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"ScreenLoupeLens";
    RegisterClassW(&wc);
    wc.lpfnWndProc = PenProc;
    wc.lpszClassName = L"ScreenLoupePen";
    RegisterClassW(&wc);

    // Поверх всех, сквозь клики, без фокуса, без кнопки на панели задач.
    const DWORD ex = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                     WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    g_dim = CreateWindowExW(ex, L"ScreenLoupeDim", L"Screen Loupe", WS_POPUP,
                            0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_dim, 0, DIM_ALPHA, LWA_ALPHA);
    // Лупа не должна видеть затемнение. MagSetWindowFilterList на Windows 11
    // возвращает успех, но окно не исключает; исключение из захвата экрана
    // Magnification API соблюдает. Своё окно лупа исключает сама.
    SetWindowDisplayAffinity(g_dim, WDA_EXCLUDEFROMCAPTURE);

    // Круг принадлежит затемнению, обводка — кругу, поэтому каждое окно всегда
    // лежит над предыдущим. Край круга проходит по середине обводки.
    g_lens = CreateWindowExW(ex, L"ScreenLoupeLens", L"", WS_POPUP,
                             0, 0, 0, 0, g_dim, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_lens, 0, 255, LWA_ALPHA);
    g_mag = CreateWindowW(WC_MAGNIFIER, L"", WS_CHILD | WS_VISIBLE,
                          0, 0, 0, 0, g_lens, nullptr, inst, nullptr);
    if (!g_mag)
        Fail(L"Не удалось создать окно лупы.");
    g_ring = CreateWindowExW(ex, L"ScreenLoupeLens", L"", WS_POPUP,
                             0, 0, 0, 0, g_lens, nullptr, inst, nullptr);
    // Обводку лупа тоже видеть не должна: при движении в круг попадала бы
    // обводка с прошлого кадра.
    SetWindowDisplayAffinity(g_ring, WDA_EXCLUDEFROMCAPTURE);
    SetZoom(1.0);
    SetRadius(RADIUS);

    // Перо, в отличие от остальных окон, ловит мышь: клики по рисунку не
    // должны доходить до программ под ним.
    g_pen = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"ScreenLoupePen", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            inst, nullptr);

    CreateThread(nullptr, 0, InputThread, nullptr, 0, &g_inputThread);
    CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}
