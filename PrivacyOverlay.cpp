#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <memory>
#include <dwmapi.h>
#include <gdiplus.h>
#include <gdipluspath.h>
#include <uxtheme.h>
#include <algorithm>
#include <mutex>
#include <thread>
#include <atomic>
#include <shlobj.h>  
#include <shlwapi.h> 
#include <gdiplustypes.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shlwapi.lib")

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;
using namespace std;

// Constants
constexpr const wchar_t* APP_NAME = L"Privacy Overlay Filter";
constexpr const wchar_t* CONFIG_FILE = L"privacy_filter_config.ini";
constexpr const wchar_t* AUTO_START_REG_KEY = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* AUTO_START_REG_VALUE = L"PrivacyFilter";
constexpr int WINDOW_WIDTH = 700;
constexpr int WINDOW_HEIGHT = 500;
constexpr int ID_THEME_LIGHT = 1001;
constexpr int ID_THEME_DARK = 1002;
constexpr int ID_ICON = 101;
constexpr int ID_ICON_ACTIVE = 102;
constexpr int ANIMATION_STEPS = 15;
constexpr int BASE_UNIT_X = 8;  
constexpr int BASE_UNIT_Y = 16; 
constexpr int CONTROL_SHADOW_SIZE = 3; 
constexpr COLORREF SHADOW_COLOR_DARK = RGB(0, 10, 20); 
constexpr COLORREF SHADOW_COLOR_LIGHT = RGB(80, 150, 145); 

// Forward declarations
class TitleBarButton;
void UpdateFilterWindow(bool forceRedraw = false);
void ToggleFilterVisibility();
void DrawGradientBackground(HDC hdc, RECT& rect, bool darkMode);
void DrawGradientBackground2(HDC hdc, RECT& rect, bool lightMode);
void DrawCustomSlider(DRAWITEMSTRUCT* dis);

enum class Theme { Light, Dark };

class GraphicsExtension : public Graphics {
public:
    GraphicsExtension(HDC hdc) : Graphics(hdc) {}

    void FillRoundRectangle(Brush* brush, Rect rect, INT radius) {
        // Create a GraphicsPath object with proper namespace
        Gdiplus::GraphicsPath path;
        INT diameter = 2 * radius;

        // Add rounded corners to path
        path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
        path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270, 90);
        path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0, 90);
        path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();

        // Use the FillPath method from the base Graphics class
        this->FillPath(brush, &path);
    }
};

struct Settings {
    int opacity = 80;
    int fadeWidth = 25;
    UINT toggleHotkey = VK_F8;
    UINT modifiers = MOD_CONTROL;
    bool startMinimized = false;
    bool startWithWindows = false;
    bool enableOnStartup = false;
    Theme currentTheme = Theme::Dark;
};

// Global variables
HINSTANCE g_hInstance = nullptr;
HWND g_hMainWnd = nullptr;
HWND g_hOpacitySlider = nullptr;
HWND g_hFadeWidthSlider = nullptr;
HWND g_hHotkeyCtrl = nullptr;
HWND g_hFilterWindow = nullptr;
HWND g_hEnableBtn = nullptr;
HWND g_hStartMinimizedChk = nullptr;
HWND g_hStartWithWindowsChk = nullptr;
HWND g_hEnableOnStartupChk = nullptr;
NOTIFYICONDATA g_notifyIconData = {};
UINT WM_TASKBAR_ICON = 0;
Settings g_settings;
std::vector<TitleBarButton> g_titleBarButtons;
bool g_isTrackingMouse = false;
ULONG_PTR g_gdiplusToken = 0;
HBRUSH g_themeBrush = nullptr;
HBRUSH g_frameBrush = nullptr;
COLORREF g_textColor = RGB(240, 240, 240);
bool g_filterEnabled = true;
std::atomic<bool> g_animatingFilter(false);
int g_currentAnimationStep = 0;
UINT_PTR g_animationTimerId = 0; 
std::recursive_mutex g_filterMutex;
int g_dpix = 96; 
int g_dpiy = 96;
HFONT g_hControlFont = nullptr;
HFONT g_hHeaderFont = nullptr;

static int ScaleX(int x) {
    return MulDiv(x, g_dpix, 96);
}

static int ScaleY(int y) {
    return MulDiv(y, g_dpiy, 96);
}

// TitleBarButton class
class TitleBarButton {
public:
    enum ButtonType { Minimize, Maximize, Close };

    TitleBarButton(ButtonType type, const RECT& rect)
        : m_type(type), m_rect(rect), m_hoverState(0), m_isHovering(false) {
    }

    void Draw(HDC hdc) const;
    bool Contains(int x, int y) const;
    bool UpdateHoverState();
    bool SetHover(bool hover);
    bool IsHovering() const { return m_isHovering; }
    void UpdateRect(const RECT& rect) { m_rect = rect; }
    ButtonType GetType() const { return m_type; }
    const RECT& GetRect() const { return m_rect; }

private:
    ButtonType m_type;
    RECT m_rect;
    int m_hoverState;
    bool m_isHovering;
};

// Forward declarations
void CreateMainWindow();
void CreateFilterWindow();
void InitializeControls(HWND hwnd);
void SaveSettings();
void LoadSettings();
void SetAutoStartWithWindows(bool enable);
bool GetAutoStartWithWindows();
void ShowContextMenu(HWND hwnd, POINT pt);
void UpdateTrayIcon(bool enabled);
void DrawCustomTitleBar(HWND hwnd, HDC hdc);
VOID CALLBACK AnimationTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
void UpdateControlsLayout(HWND hwnd);
void UpdateFonts();

static HRESULT CreateOverlayBuffer(HDC hdc, int width, int height, HDC& memDC, HBITMAP& hBitmap, void*& pBits) {
    if (!hdc) return E_FAIL;

    memDC = NULL;
    hBitmap = NULL;
    pBits = NULL;

    memDC = CreateCompatibleDC(hdc);
    if (!memDC) return E_FAIL;

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;    // 32-bit for alpha channel
    bmi.bmiHeader.biCompression = BI_RGB;

    hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBitmap) {
        DeleteDC(memDC);
        memDC = NULL;
        return E_FAIL;
    }

    HGDIOBJ oldObj = SelectObject(memDC, hBitmap);
    if (!oldObj) {
        DeleteObject(hBitmap);
        DeleteDC(memDC);
        hBitmap = NULL;
        memDC = NULL;
        return E_FAIL;
    }

    return S_OK;
}

// Function to try registering a hotkey
bool TryRegisterHotkey(HWND hwnd, UINT& modifiers, UINT& key) {
    if (modifiers == 0) {
        modifiers = MOD_CONTROL;  // Default to CTRL if no modifiers
    }

    // Try the requested hotkey first
    if (RegisterHotKey(hwnd, 1, modifiers, key)) {
        return true;
    }

    // Log the error for diagnostics
    DWORD error = GetLastError();
    wchar_t errorMsg[256];
    swprintf_s(errorMsg, L"RegisterHotKey failed with error: %d", error);
    OutputDebugString(errorMsg);
    // Try the requested hotkey first
    if (RegisterHotKey(hwnd, 1, modifiers, key)) {
        return true;
    }

    struct FallbackKey {
        UINT mod;
        UINT vk;
    };

    // Array of fallback key combinations to try
    FallbackKey fallbacks[] = {
        { MOD_CONTROL, VK_F8 },
        { MOD_CONTROL, VK_F7 },
        { MOD_ALT, VK_F8 },
        { MOD_ALT, VK_F9 },
        { MOD_CONTROL | MOD_ALT, 'P' } 
    };

    // Try each fallback option
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        const FallbackKey& fb = fallbacks[i];

        // Skip if this is the same as what we just tried
        if (fb.mod == modifiers && fb.vk == key) {
            continue;
        }

        if (RegisterHotKey(hwnd, 1, fb.mod, fb.vk)) { 
            // Update the passed modifiers and key if successful
            modifiers = fb.mod;
            key = fb.vk;

            // Create a string to show which hotkey was registered
            std::wstring modText;
            if (fb.mod & MOD_CONTROL) modText += L"Ctrl+";
            if (fb.mod & MOD_ALT) modText += L"Alt+";
            if (fb.mod & MOD_SHIFT) modText += L"Shift+";

            wchar_t keyName[32] = L"";
            if (fb.vk >= VK_F1 && fb.vk <= VK_F24) {
                swprintf_s(keyName, 32, L"F%d", fb.vk - VK_F1 + 1); 
            }
            else {
                // Handle other common keys
                switch (fb.vk) {
                case 'P': wcscpy_s(keyName, 32, L"P"); break; 
                
                default:
                    BYTE keyState[256] = { 0 };
                    WCHAR buffer[16] = { 0 };
                    ToUnicode(fb.vk, 0, keyState, buffer, 16, 0);
                    wcscpy_s(keyName, 32, buffer); 
                    break;
                }
            }

            std::wstring message = L"Couldn't register the selected hotkey. Using " + modText + keyName + L" instead.";
            MessageBox(hwnd, message.c_str(), APP_NAME, MB_ICONWARNING);
            return true;
        }
    }

    // Error message
    MessageBox(hwnd, L"Couldn't register any hotkey. Toggle functionality will not be available via hotkey.",
        APP_NAME, MB_ICONERROR);
    return false;
}

// Main window
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static Theme currentTheme = Theme::Dark;

    if (msg == WM_TASKBAR_ICON) {
        if (lParam == WM_RBUTTONDOWN || lParam == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            ShowContextMenu(hwnd, pt);
        }
        else if (lParam == WM_LBUTTONDOWN) {
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        // Initialize GDI+
        GdiplusStartupInput gdiplusStartupInput;
        GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

        // Get system DPI
        HDC hdc = GetDC(NULL);
        g_dpix = GetDeviceCaps(hdc, LOGPIXELSX);
        g_dpiy = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);

        // Create title bar buttons
        const int buttonSize = ScaleY(16), spacing = ScaleX(5), rightMargin = ScaleX(20), topMargin = ScaleY(8);
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);

        int right = clientRect.right - rightMargin;
        g_titleBarButtons.emplace_back(TitleBarButton::Close, RECT{
            right - buttonSize, topMargin, right, topMargin + buttonSize });

        right -= buttonSize + spacing;
        g_titleBarButtons.emplace_back(TitleBarButton::Maximize, RECT{
            right - buttonSize, topMargin, right, topMargin + buttonSize });

        right -= buttonSize + spacing;
        g_titleBarButtons.emplace_back(TitleBarButton::Minimize, RECT{
            right - buttonSize, topMargin, right, topMargin + buttonSize });

        UpdateFonts();

        // Initialize controls and settings
        InitializeControls(hwnd);
        LoadSettings();
        currentTheme = g_settings.currentTheme;
        SendMessage(GetDlgItem(hwnd, (currentTheme == Theme::Light) ?
            ID_THEME_LIGHT : ID_THEME_DARK), BM_SETCHECK, BST_CHECKED, 0);

        // Set animation timer
        SetTimer(hwnd, g_animationTimerId, 16, nullptr); 
        SetWindowTheme(hwnd, L"", L"");

        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);

        // Create a memory device context for double buffering
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
        HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

        // Fill the ENTIRE background including the top border
        HBRUSH backgroundBrush = CreateSolidBrush(currentTheme == Theme::Dark ?
            RGB(40, 40, 40) : RGB(240, 240, 240));
        FillRect(memDC, &rect, backgroundBrush);
        DeleteObject(backgroundBrush);


        if (currentTheme == Theme::Dark)
            DrawGradientBackground(memDC, rect, true);
        else
            DrawGradientBackground2(memDC, rect, true);

        DrawCustomTitleBar(hwnd, memDC);
        for (const auto& btn : g_titleBarButtons)
            btn.Draw(memDC);

        // Copy the off-screen buffer to the screen
        BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);

        // Clean up
        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == g_animationTimerId) {
            for (auto& btn : g_titleBarButtons) {
                bool changed = btn.UpdateHoverState();
                if (changed) {
                   
                    RECT btnRect = { 0, 0, 0, 0 };
                    btnRect.left = btn.GetRect().left;
                    btnRect.top = btn.GetRect().top;
                    btnRect.right = btn.GetRect().right;
                    btnRect.bottom = btn.GetRect().bottom;

                    // Add a bit of padding to ensure complete redraw
                    btnRect.left -= 2;
                    btnRect.top -= 2;
                    btnRect.right += 2;
                    btnRect.bottom += 2;

                    // Only invalidate the button area, not the entire window
                    InvalidateRect(hwnd, &btnRect, FALSE);
                }
            }
        }
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        for (const auto& btn : g_titleBarButtons)
            if (btn.Contains(pt.x, pt.y)) return HTCLIENT;

        if (pt.y < ScaleY(30)) return HTCAPTION;
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        bool redraw = false;

        std::vector<RECT> invalidRects;

        for (auto& btn : g_titleBarButtons) {
			bool wasHovering = btn.IsHovering(); // Add IsHovering method
            bool isNowHovering = btn.Contains(x, y);

            if (wasHovering != isNowHovering) {
                btn.SetHover(isNowHovering);
                redraw = true;

                // Get the button rect and add padding
                RECT btnRect = btn.GetRect();
                btnRect.left -= 2;
                btnRect.top -= 2;
                btnRect.right += 2;
                btnRect.bottom += 2;

                invalidRects.push_back(btnRect);
            }
        }

        if (redraw) {
            // Only invalidate specific button rects
            for (const auto& rect : invalidRects) {
                InvalidateRect(hwnd, &rect, FALSE);
            }
        }

        if (!g_isTrackingMouse) {
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            g_isTrackingMouse = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        g_isTrackingMouse = false;

        // Track which buttons need redrawing and collect their rects
        std::vector<RECT> invalidRects;
        bool redraw = false;

        for (auto& btn : g_titleBarButtons) {
            if (btn.IsHovering()) {
                btn.SetHover(false);
                redraw = true;

                // Get the button rect and add padding
                RECT btnRect = btn.GetRect();
                btnRect.left -= 2;
                btnRect.top -= 2;
                btnRect.right += 2;
                btnRect.bottom += 2;

                invalidRects.push_back(btnRect);
            }
        }

        if (redraw) {
            // Only invalidate specific button rects
            for (const auto& rect : invalidRects) {
                InvalidateRect(hwnd, &rect, FALSE);
            }
        }

        return 0;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        for (const auto& btn : g_titleBarButtons) {
            if (btn.Contains(x, y)) {
                switch (btn.GetType()) {
                case TitleBarButton::Minimize:
                    ShowWindow(hwnd, SW_MINIMIZE);
                    break;
                case TitleBarButton::Maximize:
                    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                    break;
                case TitleBarButton::Close:
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                    break;
                }
                break;
            }
        }
        return 0;
    }

    case WM_SIZE: {
        // Update title bar buttons position
        const int buttonSize = ScaleY(16), spacing = ScaleX(5), rightMargin = ScaleX(20), topMargin = ScaleY(8);
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);

        int right = clientRect.right - rightMargin;
        if (g_titleBarButtons.size() >= 3) {
            g_titleBarButtons[0].UpdateRect({ right - buttonSize, topMargin,
                                            right, topMargin + buttonSize });

            right -= buttonSize + spacing;
            g_titleBarButtons[1].UpdateRect({ right - buttonSize, topMargin,
                                            right, topMargin + buttonSize });

            right -= buttonSize + spacing;
            g_titleBarButtons[2].UpdateRect({ right - buttonSize, topMargin,
                                            right, topMargin + buttonSize });
        }

        // Update control layout for the new size
        UpdateControlsLayout(hwnd);

        // Force redraw
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);

        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = ScaleX(600); // Minimum width
        mmi->ptMinTrackSize.y = ScaleY(450); // Minimum height
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpix = HIWORD(wParam);
        g_dpiy = LOWORD(wParam);

        // Update fonts for the new DPI
        UpdateFonts();

        // Reposition the window based on suggested rect
        RECT* suggestedRect = (RECT*)lParam;
        SetWindowPos(hwnd, NULL,
            suggestedRect->left, suggestedRect->top,
            suggestedRect->right - suggestedRect->left,
            suggestedRect->bottom - suggestedRect->top,
            SWP_NOZORDER);

        UpdateControlsLayout(hwnd);
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if ((HWND)lParam == g_hHotkeyCtrl && code == EN_CHANGE) {
            // User changed the hotkey, try to register it immediately
            UINT hotkey = (UINT)SendMessage(g_hHotkeyCtrl, HKM_GETHOTKEY, 0, 0);
            UINT key = LOBYTE(hotkey);
            UINT modifiers = 0;
            BYTE hotkeyModifiers = HIBYTE(hotkey);

            if (hotkeyModifiers & HOTKEYF_ALT) modifiers |= MOD_ALT;
            if (hotkeyModifiers & HOTKEYF_CONTROL) modifiers |= MOD_CONTROL;
            if (hotkeyModifiers & HOTKEYF_SHIFT) modifiers |= MOD_SHIFT;

            // Unregister existing hotkey
            UnregisterHotKey(hwnd, 1);

            // Try to register the new hotkey
            if (TryRegisterHotkey(hwnd, modifiers, key)) {
                g_settings.toggleHotkey = key;
                g_settings.modifiers = modifiers;
                SaveSettings();
            }
            return 0;
        }
        if (id == ID_THEME_LIGHT || id == ID_THEME_DARK) {
            currentTheme = (id == ID_THEME_LIGHT) ? Theme::Light : Theme::Dark;
            g_settings.currentTheme = currentTheme;
            SaveSettings();

                    if (dis->hwndItem == g_hOpacitySlider || dis->hwndItem == g_hFadeWidthSlider) {
            // Custom slider drawing
            DrawCustomSlider(dis);
            return TRUE;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam); 
    }
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

void DrawCustomSlider(DRAWITEMSTRUCT* dis) {
    if (!dis) return;

    HDC hdc = dis->hDC;
    RECT rect = dis->rcItem;
    HWND hwnd = dis->hwndItem;

    // Get slider position and range
    DWORD pos = static_cast<DWORD>(SendMessage(hwnd, TBM_GETPOS, 0, 0));
    DWORD min = static_cast<DWORD>(SendMessage(hwnd, TBM_GETRANGEMIN, 0, 0));
    DWORD max = static_cast<DWORD>(SendMessage(hwnd, TBM_GETRANGEMAX, 0, 0));

    // Calculate dimensions proportionally to control size
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    int thumbWidth = (std::min)(ScaleX(20), width / 10);
    int thumbHeight = (std::min)(ScaleY(20), height);
    int trackHeight = (std::min)(ScaleY(6), height / 3);
    int trackY = rect.top + (height - trackHeight) / 2;
    int trackLeft = rect.left + thumbWidth / 2;
    int trackRight = rect.right - thumbWidth / 2;

    // Safety check for division by zero
    if (max == min) max = min + 1;

    int thumbX = trackLeft + (pos - min) * (trackRight - trackLeft) / (max - min);

    // Colors based on theme
    COLORREF trackColor = g_settings.currentTheme == Theme::Dark ?
        RGB(50, 80, 100) : RGB(150, 220, 215);
    COLORREF fillColor = g_settings.currentTheme == Theme::Dark ?
        RGB(80, 140, 180) : RGB(80, 180, 175);
    COLORREF thumbColor = g_settings.currentTheme == Theme::Dark ?
        RGB(100, 180, 220) : RGB(50, 150, 145);

    // Create a memory DC for double buffering
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

    // Clear background
    HBRUSH bgBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    FillRect(memDC, &rect, bgBrush);

    // Draw track background
    HBRUSH trackBrush = CreateSolidBrush(trackColor);
    RECT trackRect = { trackLeft, trackY, trackRight, trackY + trackHeight };
    FillRect(memDC, &trackRect, trackBrush);
    DeleteObject(trackBrush);

    // Draw filled portion
    HBRUSH fillBrush = CreateSolidBrush(fillColor);
    RECT fillRect = { trackLeft, trackY, thumbX, trackY + trackHeight };
    FillRect(memDC, &fillRect, fillBrush);
    DeleteObject(fillBrush);

    // Draw thumb with rounded corners
    HBRUSH thumbBrush = CreateSolidBrush(thumbColor);
    RECT thumbRect = { thumbX - thumbWidth / 2, trackY - (thumbHeight - trackHeight) / 2,
                     thumbX + thumbWidth / 2, trackY + trackHeight + (thumbHeight - trackHeight) / 2 };

    // Draw rounded thumb with GDI+
    GraphicsExtension graphics(memDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush gdiBrush(Color(255, GetRValue(thumbColor), GetGValue(thumbColor), GetBValue(thumbColor)));

    Rect roundThumbRect(thumbRect.left, thumbRect.top,
        thumbRect.right - thumbRect.left,
        thumbRect.bottom - thumbRect.top);

    // Create a rounded rectangle path and fill it
    Gdiplus::GraphicsPath path;
    int radius = 5;
    int diameter = 2 * radius;

    // Add rounded corners to path
    path.AddArc(roundThumbRect.X, roundThumbRect.Y, diameter, diameter, 180, 90);
    path.AddArc(roundThumbRect.X + roundThumbRect.Width - diameter, roundThumbRect.Y, diameter, diameter, 270, 90);
    path.AddArc(roundThumbRect.X + roundThumbRect.Width - diameter, roundThumbRect.Y + roundThumbRect.Height - diameter, diameter, diameter, 0, 90);
    path.AddArc(roundThumbRect.X, roundThumbRect.Y + roundThumbRect.Height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();

    graphics.FillPath(&gdiBrush, &path);

    DeleteObject(thumbBrush);

    // Copy from memory DC to the original DC
    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    // Clean up
    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
}

// Create the main window
void CreateMainWindow() {
    WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = MainWndProc;
    wcex.hInstance = g_hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"PrivacyFilterMainClass";
    wcex.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(ID_ICON));
    wcex.hIconSm = LoadIcon(g_hInstance, MAKEINTRESOURCE(ID_ICON));
    RegisterClassEx(&wcex);

    // Calculate DPI-aware window size
    int scaledWidth = ScaleX(WINDOW_WIDTH);
    int scaledHeight = ScaleY(WINDOW_HEIGHT);

    // Calculate centered position
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - scaledWidth) / 2;
    int posY = (screenHeight - scaledHeight) / 2;

    g_hMainWnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        L"PrivacyFilterMainClass",
        APP_NAME,
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,  // Use WS_POPUP to remove default window frame
        posX, posY,
        scaledWidth, scaledHeight,
        nullptr,
        nullptr,
        g_hInstance,
        nullptr
    );

    // Enable modern window frame with shadow but no border
    MARGINS margins = { 0, 0, 0, 0 }; // Set all margins to 0
    DwmExtendFrameIntoClientArea(g_hMainWnd, &margins);

    // Enable modern window transitions
    BOOL enableTransitions = TRUE;
    DwmSetWindowAttribute(g_hMainWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &enableTransitions, sizeof(enableTransitions));

    // Set window corner radius (Windows 11 feature)
    // Only try to use these if available (won't crash on Windows 10)
#ifndef DWMWCP_ROUND
    constexpr int DWMWCP_ROUND = 2;
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
#endif

    // Try to set rounded corners, will be ignored on Windows 10
    int cornerPreference = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hMainWnd, DWMWA_WINDOW_CORNER_PREFERENCE,
        &cornerPreference, sizeof(cornerPreference));

    // Set window icons
    HICON hSmallIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(ID_ICON));
    HICON hBigIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(ID_ICON));
    SendMessage(g_hMainWnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmallIcon);
    SendMessage(g_hMainWnd, WM_SETICON, ICON_BIG, (LPARAM)hBigIcon);

    // Setup tray icon with modern settings
    g_notifyIconData = {};
    g_notifyIconData.cbSize = sizeof(NOTIFYICONDATA);
    g_notifyIconData.hWnd = g_hMainWnd;
    g_notifyIconData.uID = ID_ICON;
    g_notifyIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_notifyIconData.uCallbackMessage = WM_TASKBAR_ICON;
    g_notifyIconData.hIcon = hSmallIcon;
    wcscpy_s(g_notifyIconData.szTip, L"Privacy Filter");
    Shell_NotifyIcon(NIM_ADD, &g_notifyIconData);

    // Set version for modern tray icon behavior (Win10+)
    g_notifyIconData.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &g_notifyIconData);

    UpdateTrayIcon(g_filterEnabled);
}

void InitializeControls(HWND hwnd) {
    // Get client rect for scaling calculations
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int clientWidth = clientRect.right - clientRect.left;

    // Calculate DPI-aware metrics
    const int labelWidth = ScaleX(150);
    const int controlWidth = ScaleX(200);
    const int leftMargin = ScaleX(50);
    const int topMargin = ScaleY(80);
    const int verticalSpacing = ScaleY(40);

    // Theme selection with modern styling
    HWND hLightTheme = CreateWindowEx(WS_EX_TRANSPARENT, L"BUTTON", L"Light Mode",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        leftMargin, topMargin, ScaleX(100), ScaleY(30), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_THEME_LIGHT)),
        g_hInstance, nullptr);
    SendMessage(hLightTheme, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hLightRgn = CreateRoundRectRgn(0, 0, ScaleX(200), ScaleY(30), ScaleX(4), ScaleY(8));
    SetWindowRgn(hLightTheme, hLightRgn, TRUE);

    HWND hDarkTheme = CreateWindowEx(WS_EX_TRANSPARENT, L"BUTTON", L"Dark Mode",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        leftMargin + ScaleX(120), topMargin, ScaleX(100), ScaleY(30), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_THEME_DARK)),
        g_hInstance, nullptr);
    SendMessage(hDarkTheme, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hDarkRgn = CreateRoundRectRgn(0, 0, ScaleX(200), ScaleY(30), ScaleX(4), ScaleY(8));
    SetWindowRgn(hDarkTheme, hDarkRgn, TRUE);

    int controlY = topMargin + verticalSpacing;

    // Opacity slider with modern styling
    HWND hOpacityLabel = CreateWindowEx(WS_EX_TRANSPARENT, L"STATIC", L"Opacity:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        leftMargin, controlY, labelWidth, ScaleY(50),
        hwnd, nullptr, g_hInstance, nullptr);
    SendMessage(hOpacityLabel, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hOpacityLabelRgn = CreateRoundRectRgn(0, 0, ScaleX(300), ScaleY(30), ScaleX(30), ScaleY(15));
    SetWindowRgn(hOpacityLabel, hOpacityLabelRgn, TRUE);

    g_hOpacitySlider = CreateWindowEx(WS_EX_TRANSPARENT, TRACKBAR_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_TOOLTIPS | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
        leftMargin + labelWidth + ScaleX(10), controlY, controlWidth, ScaleY(50),
        hwnd, nullptr, g_hInstance, nullptr);
    SendMessage(g_hOpacitySlider, TBM_SETRANGEMAX, FALSE, 100);
    SendMessage(g_hOpacitySlider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessage(g_hOpacitySlider, TBM_SETTICFREQ, 5, 0);
    SendMessage(g_hOpacitySlider, TBM_SETPAGESIZE, 0, 10);
    HRGN hOpacitySliderRgn = CreateRoundRectRgn(0, 0, controlWidth, ScaleY(30), ScaleX(8), ScaleY(8));
    SetWindowRgn(g_hOpacitySlider, hOpacitySliderRgn, TRUE);

    controlY += verticalSpacing;

    // Fade Width slider with modern styling
    HWND hFadeWidthLabel = CreateWindowEx(WS_EX_TRANSPARENT, L"STATIC", L"Fade Width:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        leftMargin, controlY, labelWidth, ScaleY(30),
        hwnd, nullptr, g_hInstance, nullptr);
    SendMessage(hFadeWidthLabel, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hFadeWidthLabelRgn = CreateRoundRectRgn(0, 0, labelWidth, ScaleY(30), ScaleX(5), ScaleY(15));
    SetWindowRgn(hFadeWidthLabel, hFadeWidthLabelRgn, TRUE);

    g_hFadeWidthSlider = CreateWindowEx(WS_EX_TRANSPARENT, TRACKBAR_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_TOOLTIPS | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
        leftMargin + labelWidth + ScaleX(10), controlY, controlWidth, ScaleY(30),
        hwnd, nullptr, g_hInstance, nullptr);
    SendMessage(g_hFadeWidthSlider, TBM_SETRANGEMAX, FALSE, 50);
    SendMessage(g_hFadeWidthSlider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessage(g_hFadeWidthSlider, TBM_SETTICFREQ, 10, 0);
    SendMessage(g_hFadeWidthSlider, TBM_SETPAGESIZE, 0, 10);
    HRGN hFadeWidthSliderRgn = CreateRoundRectRgn(0, 0, controlWidth, ScaleY(30), ScaleX(8), ScaleY(8));
    SetWindowRgn(g_hFadeWidthSlider, hFadeWidthSliderRgn, TRUE);

    controlY += verticalSpacing;

    // Toggle Hotkey with modern styling
    HWND hHotkeyLabel = CreateWindowEx(WS_EX_TRANSPARENT, L"STATIC", L"Toggle Hotkey:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        leftMargin, controlY, labelWidth, ScaleY(30),
        hwnd, nullptr, g_hInstance, nullptr);
    SendMessage(hHotkeyLabel, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hHotkeyLabelRgn = CreateRoundRectRgn(0, 0, labelWidth, ScaleY(30), ScaleX(15), ScaleY(15));
    SetWindowRgn(hHotkeyLabel, hHotkeyLabelRgn, TRUE);

    g_hHotkeyCtrl = CreateWindowEx(WS_EX_TRANSPARENT, HOTKEY_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
        leftMargin + labelWidth + ScaleX(20), controlY, controlWidth, ScaleY(30),
        hwnd, nullptr, g_hInstance, nullptr);
    SendMessage(g_hHotkeyCtrl, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hHotkeyCtrlRgn = CreateRoundRectRgn(0, 0, ScaleX(100), ScaleY(30), ScaleX(15), ScaleY(15));
    SetWindowRgn(g_hHotkeyCtrl, hHotkeyCtrlRgn, TRUE);

    controlY += verticalSpacing;

    g_hEnableBtn = CreateWindowEx(WS_EX_TRANSPARENT, L"BUTTON",
        g_filterEnabled ? L"Disable Filter" : L"Enable Filter",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_FLAT | WS_TABSTOP,
        leftMargin, controlY, controlWidth + labelWidth + ScaleX(30), ScaleY(40),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(101)),
        g_hInstance, nullptr);
    SendMessage(g_hEnableBtn, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    // Change this line to increase the corner radius:
    HRGN hBtnRgn = CreateRoundRectRgn(0, 0, controlWidth + labelWidth + ScaleX(30), ScaleY(40), ScaleX(8), ScaleY(8)); 
    SetWindowRgn(g_hEnableBtn, hBtnRgn, TRUE);

    controlY += verticalSpacing + ScaleY(20);

    // Start Minimized checkbox with modern styling
    g_hStartMinimizedChk = CreateWindowEx(WS_EX_TRANSPARENT, L"BUTTON", L"Start minimized",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_FLAT | WS_TABSTOP,
        leftMargin, controlY, ScaleX(200), ScaleY(30),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(102)),
        g_hInstance, nullptr);
    SendMessage(g_hStartMinimizedChk, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hStartMinRgn = CreateRoundRectRgn(0, 0, ScaleX(200), ScaleY(30), ScaleX(10), ScaleY(10));
    SetWindowRgn(g_hStartMinimizedChk, hStartMinRgn, TRUE);

    // Start with Windows checkbox with modern styling
    g_hStartWithWindowsChk = CreateWindowEx(WS_EX_TRANSPARENT, L"BUTTON", L"Start with Windows",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_FLAT | WS_TABSTOP,
        leftMargin + ScaleX(250), controlY, ScaleX(200), ScaleY(30),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(103)),
        g_hInstance, nullptr);
    SendMessage(g_hStartWithWindowsChk, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hStartWinRgn = CreateRoundRectRgn(0, 0, ScaleX(200), ScaleY(30), ScaleX(10), ScaleY(10));
    SetWindowRgn(g_hStartWithWindowsChk, hStartWinRgn, TRUE);

    controlY += verticalSpacing;

    // Enable on Startup checkbox with modern styling
    g_hEnableOnStartupChk = CreateWindowEx(WS_EX_TRANSPARENT, L"BUTTON", L"Enable filter on startup",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_FLAT | WS_TABSTOP,
        leftMargin, controlY, ScaleX(400), ScaleY(30),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(104)),
        g_hInstance, nullptr);
    SendMessage(g_hEnableOnStartupChk, WM_SETFONT, (WPARAM)g_hControlFont, TRUE);
    HRGN hEnableStartupRgn = CreateRoundRectRgn(0, 0, ScaleX(400), ScaleY(30), ScaleX(10), ScaleY(10));
    SetWindowRgn(g_hEnableOnStartupChk, hEnableStartupRgn, TRUE);

    // Configure controls with current settings
    SendMessage(g_hOpacitySlider, TBM_SETPOS, TRUE, g_settings.opacity);
    SendMessage(g_hFadeWidthSlider, TBM_SETPOS, TRUE, g_settings.fadeWidth);
    SendMessage(g_hHotkeyCtrl, HKM_SETHOTKEY, MAKEWORD(g_settings.toggleHotkey, g_settings.modifiers >> 8), 0);
    SendMessage(g_hStartMinimizedChk, BM_SETCHECK, g_settings.startMinimized ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(g_hStartWithWindowsChk, BM_SETCHECK, g_settings.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(g_hEnableOnStartupChk, BM_SETCHECK, g_settings.enableOnStartup ? BST_CHECKED : BST_UNCHECKED, 0);

    // Apply visual styles to all controls for modern appearance
    SetWindowTheme(g_hOpacitySlider, L"Explorer", NULL);
    SetWindowTheme(g_hFadeWidthSlider, L"Explorer", NULL);
    SetWindowTheme(g_hHotkeyCtrl, L"Explorer", NULL);
    SetWindowTheme(g_hEnableBtn, L"Explorer", NULL);
    SetWindowTheme(g_hStartMinimizedChk, L"Explorer", NULL);
    SetWindowTheme(g_hStartWithWindowsChk, L"Explorer", NULL);
    SetWindowTheme(g_hEnableOnStartupChk, L"Explorer", NULL);

    // Force redraw of child controls
    wchar_t className[256];
    HWND childHwnd = GetWindow(hwnd, GW_CHILD);
    while (childHwnd) {
        if (GetClassName(childHwnd, className, sizeof(className) / sizeof(wchar_t)) > 0) {
            if (wcscmp(className, L"STATIC") == 0) {
                // Force redraw with current theme colors
                InvalidateRect(childHwnd, NULL, TRUE);
            }
        }
        childHwnd = GetWindow(childHwnd, GW_HWNDNEXT);
    }
}

static void AddControlShadow(HWND hwnd, int width, int height, int shadowSize) {
    // Create a shadow HWND behind the control
    HWND shadowHwnd = CreateWindowEx(
        WS_EX_TRANSPARENT | WS_EX_LAYERED,
        L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        shadowSize, shadowSize, width, height,
        GetParent(hwnd), nullptr, g_hInstance, nullptr);

    // Store the shadow HWND as a property of the main control
    SetProp(hwnd, L"ShadowHwnd", shadowHwnd);

    // Set the shadow color based on theme
    COLORREF shadowColor = g_settings.currentTheme == Theme::Dark ?
        SHADOW_COLOR_DARK : SHADOW_COLOR_LIGHT;
    SetLayeredWindowAttributes(shadowHwnd, 0, 180, LWA_ALPHA); // 70% opacity

    // Position the shadow behind the control
    RECT rect;
    GetWindowRect(hwnd, &rect);
    MapWindowPoints(NULL, GetParent(hwnd), (LPPOINT)&rect, 2);
    SetWindowPos(shadowHwnd, hwnd, rect.left + shadowSize, rect.top + shadowSize,
        width, height, SWP_NOACTIVATE);
}

void CreateFilterWindow() {
    // Unregister the class if it already exists to avoid issues
    UnregisterClass(L"PrivacyFilterClass", g_hInstance);

    WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = FilterWndProc;
    wcex.hInstance = g_hInstance;
    wcex.hCursor = nullptr; // No cursor needed for overlay
    wcex.lpszClassName = L"PrivacyFilterClass";
    RegisterClassEx(&wcex);

    // Get virtual screen metrics to cover all monitors
    int virtualScreenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualScreenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Destroy any existing filter window first
    if (g_hFilterWindow && IsWindow(g_hFilterWindow)) {
        DestroyWindow(g_hFilterWindow);
        g_hFilterWindow = NULL;
    }

    // Create the filter window with proper extended styles
    g_hFilterWindow = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"PrivacyFilterClass",
        nullptr,
        WS_POPUP,
        virtualScreenLeft, virtualScreenTop,
        virtualScreenWidth, virtualScreenHeight,
        nullptr,
        nullptr,
        g_hInstance,
        nullptr
    );

    if (!g_hFilterWindow) {
        MessageBox(NULL, L"Failed to create filter window", APP_NAME, MB_ICONERROR);
        return;
    }

    // Set initial alpha to 0 (fully transparent)
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 0, 0 };
    UpdateLayeredWindow(g_hFilterWindow, NULL, NULL, NULL, NULL, NULL, 0, &blend, ULW_ALPHA);
}

void ToggleFilterVisibility() {
    // Prevent multiple toggles during animation
    if (g_animatingFilter.exchange(true)) return;

    // Reset animation step counter
    g_currentAnimationStep = 0;

    // Initialize filter window if not already done
    if (!IsWindow(g_hFilterWindow)) {
        CreateFilterWindow();
    }

    // Toggle filter state
    g_filterEnabled = !g_filterEnabled;

    // Update UI state based on new filter state
    if (g_filterEnabled) {
        // Update filter window position for multi-monitor support
        int virtualScreenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virtualScreenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        // Set position and size to cover all monitors
        SetWindowPos(g_hFilterWindow, HWND_TOPMOST,
            virtualScreenLeft, virtualScreenTop,
            virtualScreenWidth, virtualScreenHeight,
            SWP_NOACTIVATE);

        // Show without activating
        ShowWindow(g_hFilterWindow, SW_SHOWNOACTIVATE);

        // Update UI state
        if (IsWindow(g_hEnableBtn)) {
            SendMessage(g_hEnableBtn, WM_SETTEXT, 0, (LPARAM)L"Disable Filter");
        }
        UpdateTrayIcon(true);
    }

    // Force immediate redraw
    UpdateFilterWindow(true);

    // Stop any existing timer first
    if (g_animationTimerId != 0) {
        KillTimer(g_hMainWnd, g_animationTimerId);
        g_animationTimerId = 0;
    }

    // Start smooth animation
    g_animationTimerId = SetTimer(g_hMainWnd, 100, 16, AnimationTimerProc);

    // If timer creation failed, ensure we clean up properly
    if (g_animationTimerId == 0) {
        g_animatingFilter = false;
        if (!g_filterEnabled) {
            ShowWindow(g_hFilterWindow, SW_HIDE);
            if (IsWindow(g_hEnableBtn)) {
                SendMessage(g_hEnableBtn, WM_SETTEXT, 0, (LPARAM)L"Enable Filter");
            }
            UpdateTrayIcon(false);
        }
    }
}

// Update the filter overlay window
void UpdateFilterWindow(bool forceRedraw) {
    if (!g_hFilterWindow || !IsWindow(g_hFilterWindow)) return;

    try {
        // Lock to prevent race conditions during drawing
        std::unique_lock<std::recursive_mutex> lock(g_filterMutex);

        // For multi-monitor support, get the entire virtual screen area
        int virtualScreenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virtualScreenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        // Validate screen dimensions to prevent potential issues
        if (virtualScreenWidth <= 0 || virtualScreenHeight <= 0) return;

        // Update the filter window size and position
        SetWindowPos(g_hFilterWindow, HWND_TOPMOST,
            virtualScreenLeft, virtualScreenTop,
            virtualScreenWidth, virtualScreenHeight,
            SWP_NOACTIVATE);

        // Force immediate redraw with optimized invalidation
        if (forceRedraw) {
            RedrawWindow(g_hFilterWindow, NULL, NULL,
                RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
        else {
            InvalidateRect(g_hFilterWindow, nullptr, FALSE);
        }
    }
    catch (...) {
        // Log or handle the exception as needed
        // Silent catch to prevent crashes
    }
}

// Save settings to the configuration file
void SaveSettings() {
    // Get current hotkey settings from control
    UINT hotkey = (UINT)SendMessage(g_hHotkeyCtrl, HKM_GETHOTKEY, 0, 0);
    g_settings.toggleHotkey = LOBYTE(hotkey);

    // Convert HOTKEYF_* flags to MOD_* flags properly
    BYTE modifiers = HIBYTE(hotkey);
    g_settings.modifiers = 0;
    if (modifiers & HOTKEYF_ALT) g_settings.modifiers |= MOD_ALT;
    if (modifiers & HOTKEYF_CONTROL) g_settings.modifiers |= MOD_CONTROL;
    if (modifiers & HOTKEYF_SHIFT) g_settings.modifiers |= MOD_SHIFT;

    // First unregister the existing hotkey
    UnregisterHotKey(g_hMainWnd, 1);

    // Try to register the hotkey with fallbacks
    if (TryRegisterHotkey(g_hMainWnd, g_settings.modifiers, g_settings.toggleHotkey)) {
        // If successful, update the control to match what was actually registered
        BYTE hotkeyModifiers = 0;
        if (g_settings.modifiers & MOD_ALT) hotkeyModifiers |= HOTKEYF_ALT;
        if (g_settings.modifiers & MOD_CONTROL) hotkeyModifiers |= HOTKEYF_CONTROL;
        if (g_settings.modifiers & MOD_SHIFT) hotkeyModifiers |= HOTKEYF_SHIFT;

        SendMessage(g_hHotkeyCtrl, HKM_SETHOTKEY, MAKEWORD(g_settings.toggleHotkey, hotkeyModifiers), 0);
    }

    // Create configuration directory if it doesn't exist
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring configDir = std::wstring(appDataPath) + L"\\PrivacyFilter";
        CreateDirectory(configDir.c_str(), NULL);

        std::wstring configPath = configDir + L"\\" + CONFIG_FILE;

        // Save settings to file with error handling
        std::wofstream file(configPath);
        if (file.is_open()) {
            file << L"opacity=" << g_settings.opacity << std::endl;
            file << L"fadeWidth=" << g_settings.fadeWidth << std::endl;
            file << L"toggleHotkey=" << g_settings.toggleHotkey << std::endl;
            file << L"modifiers=" << g_settings.modifiers << std::endl;
            file << L"startMinimized=" << g_settings.startMinimized << std::endl;
            file << L"startWithWindows=" << g_settings.startWithWindows << std::endl;
            file << L"enableOnStartup=" << g_settings.enableOnStartup << std::endl;
            file << L"theme=" << static_cast<int>(g_settings.currentTheme) << std::endl;
            file.close();
        }
        else {
            // Inform user if settings couldn't be saved
            MessageBox(g_hMainWnd, L"Couldn't save settings to configuration file.",
                APP_NAME, MB_ICONWARNING);
        }
    }
    else {
        // Fall back to current directory if AppData is not available
        std::wofstream file(CONFIG_FILE);
        if (file.is_open()) {
            file << L"opacity=" << g_settings.opacity << std::endl;
            file << L"fadeWidth=" << g_settings.fadeWidth << std::endl;
            file << L"toggleHotkey=" << g_settings.toggleHotkey << std::endl;
            file << L"modifiers=" << g_settings.modifiers << std::endl;
            file << L"startMinimized=" << g_settings.startMinimized << std::endl;
            file << L"startWithWindows=" << g_settings.startWithWindows << std::endl;
            file << L"enableOnStartup=" << g_settings.enableOnStartup << std::endl;
            file << L"theme=" << static_cast<int>(g_settings.currentTheme) << std::endl;
            file.close();
        }
    }
}

// Load settings from the configuration file
void LoadSettings() {
    // First try to load from appdata location
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring configDir = std::wstring(appDataPath) + L"\\PrivacyFilter";
        std::wstring configPath = configDir + L"\\" + CONFIG_FILE;

        // Try appdata path first, fall back to current directory
        std::wifstream file(configPath);
        if (!file.is_open()) {
            file.open(CONFIG_FILE);
        }

        if (file.is_open()) {
            std::wstring line;
            while (std::getline(file, line)) {
                size_t equalsPos = line.find(L'=');
                if (equalsPos != std::wstring::npos) {
                    std::wstring key = line.substr(0, equalsPos);
                    std::wstring value = line.substr(equalsPos + 1);

                    try {
                        if (key == L"opacity") g_settings.opacity = std::stoi(value);
                        else if (key == L"fadeWidth") g_settings.fadeWidth = std::stoi(value);
                        else if (key == L"toggleHotkey") g_settings.toggleHotkey = std::stoi(value);
                        else if (key == L"modifiers") g_settings.modifiers = std::stoi(value);
                        else if (key == L"startMinimized") g_settings.startMinimized = std::stoi(value) != 0;
                        else if (key == L"startWithWindows") g_settings.startWithWindows = std::stoi(value) != 0;
                        else if (key == L"enableOnStartup") g_settings.enableOnStartup = std::stoi(value) != 0;
                        else if (key == L"theme") g_settings.currentTheme = static_cast<Theme>(std::stoi(value));
                    }
                    catch (const std::exception&) {
                        // Skip invalid values and continue loading
                        continue;
                    }
                }
            }
            file.close();
        }
    }
    else {
        // Try current directory if appdata access failed
        std::wifstream file(CONFIG_FILE);
        if (file.is_open()) {
            // Read settings (same as above)
            std::wstring line;
            while (std::getline(file, line)) {
                size_t equalsPos = line.find(L'=');
                if (equalsPos != std::wstring::npos) {
                    std::wstring key = line.substr(0, equalsPos);
                    std::wstring value = line.substr(equalsPos + 1);

                    try {
                        if (key == L"opacity") g_settings.opacity = std::stoi(value);
                        else if (key == L"fadeWidth") g_settings.fadeWidth = std::stoi(value);
                        else if (key == L"toggleHotkey") g_settings.toggleHotkey = std::stoi(value);
                        else if (key == L"modifiers") g_settings.modifiers = std::stoi(value);
                        else if (key == L"startMinimized") g_settings.startMinimized = std::stoi(value) != 0;
                        else if (key == L"startWithWindows") g_settings.startWithWindows = std::stoi(value) != 0;
                        else if (key == L"enableOnStartup") g_settings.enableOnStartup = std::stoi(value) != 0;
                        else if (key == L"theme") g_settings.currentTheme = static_cast<Theme>(std::stoi(value));
                    }
                    catch (const std::exception&) {
                        // Skip invalid values and continue loading
                        continue;
                    }
                }
            }
            file.close();
        }
    }

    // Validate loaded settings and set defaults if needed
    g_settings.opacity = (std::max)(0, (std::min)(g_settings.opacity, 100));
    g_settings.fadeWidth = (std::max)(0, (std::min)(g_settings.fadeWidth, 50));

    // Make sure we have valid hotkey settings
    if (g_settings.toggleHotkey == 0) {
        g_settings.toggleHotkey = VK_F8;
        g_settings.modifiers = MOD_CONTROL;
    }

    // Make sure modifiers aren't empty (Windows requires at least one modifier for hotkeys)
    if (g_settings.modifiers == 0) {
        g_settings.modifiers = MOD_CONTROL;
    }

    // Match actual autostart state with settings
    g_settings.startWithWindows = GetAutoStartWithWindows();

    // Setup hotkey if control exists
    if (g_hHotkeyCtrl && IsWindow(g_hHotkeyCtrl)) {
        // Unregister any existing hotkey first
        UnregisterHotKey(g_hMainWnd, 1);

        // Try to register the hotkey with fallback options if needed
        bool result = TryRegisterHotkey(g_hMainWnd, g_settings.modifiers, g_settings.toggleHotkey);

        // Convert MOD_* flags to HOTKEYF_* flags for the UI control
        BYTE hotkeyModifiers = 0;
        if (g_settings.modifiers & MOD_ALT) hotkeyModifiers |= HOTKEYF_ALT;
        if (g_settings.modifiers & MOD_CONTROL) hotkeyModifiers |= HOTKEYF_CONTROL;
        if (g_settings.modifiers & MOD_SHIFT) hotkeyModifiers |= HOTKEYF_SHIFT;

        // Update the hotkey control to show the actually registered hotkey
        SendMessage(g_hHotkeyCtrl, HKM_SETHOTKEY, MAKEWORD(g_settings.toggleHotkey, hotkeyModifiers), 0);

        // If registration failed despite fallbacks, show an error
        if (!result) {
            MessageBox(g_hMainWnd,
                L"Failed to register any hotkey. The toggle functionality may not work with keyboard shortcuts.",
                APP_NAME, MB_ICONWARNING);
        }
    }

    // If controls exist, update them with current settings
    if (g_hOpacitySlider && IsWindow(g_hOpacitySlider)) {
        SendMessage(g_hOpacitySlider, TBM_SETPOS, TRUE, g_settings.opacity);
    }

    if (g_hFadeWidthSlider && IsWindow(g_hFadeWidthSlider)) {
        SendMessage(g_hFadeWidthSlider, TBM_SETPOS, TRUE, g_settings.fadeWidth);
    }

    if (g_hStartMinimizedChk && IsWindow(g_hStartMinimizedChk)) {
        SendMessage(g_hStartMinimizedChk, BM_SETCHECK, g_settings.startMinimized ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    if (g_hStartWithWindowsChk && IsWindow(g_hStartWithWindowsChk)) {
        SendMessage(g_hStartWithWindowsChk, BM_SETCHECK, g_settings.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    if (g_hEnableOnStartupChk && IsWindow(g_hEnableOnStartupChk)) {
        SendMessage(g_hEnableOnStartupChk, BM_SETCHECK, g_settings.enableOnStartup ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

// Set or remove the application from Windows startup
void SetAutoStartWithWindows(bool enable) {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, AUTO_START_REG_KEY, 0, KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS) return;

    if (enable) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileName(nullptr, exePath, MAX_PATH);

        // Add quotes around path to handle spaces properly
        std::wstring quotedPath = L"\"" + std::wstring(exePath) + L"\"";

        RegSetValueEx(hKey, AUTO_START_REG_VALUE, 0, REG_SZ,
            (BYTE*)quotedPath.c_str(),
            (DWORD)((quotedPath.length() + 1) * sizeof(wchar_t)));
    }
    else {
        RegDeleteValue(hKey, AUTO_START_REG_VALUE);
    }
    RegCloseKey(hKey);
}

// Check if the application is set to start with Windows
bool GetAutoStartWithWindows() {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, AUTO_START_REG_KEY, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return false;

    wchar_t value[MAX_PATH] = { 0 };
    DWORD valueSize = MAX_PATH * sizeof(wchar_t);
    DWORD valueType = 0;

    result = RegQueryValueEx(hKey, AUTO_START_REG_VALUE, 0, &valueType, (BYTE*)value, &valueSize);
    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS) return false;

    // Verify that the entry points to the current executable
    wchar_t exePath[MAX_PATH];
    GetModuleFileName(nullptr, exePath, MAX_PATH);

    std::wstring regValue = value;
    std::wstring currentExe = exePath;

    // Remove quotes if present
    regValue.erase(std::remove(regValue.begin(), regValue.end(), L'\"'), regValue.end());

    return _wcsicmp(regValue.c_str(), currentExe.c_str()) == 0;
}

// Show the context menu for the tray icon
void ShowContextMenu(HWND hwnd, POINT pt) {
    if (!hwnd) return;

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // Add menu items with modern icons
    AppendMenu(hMenu, MF_STRING, 1, g_filterEnabled ? L"Disable Filter" : L"Enable Filter");
    AppendMenu(hMenu, MF_STRING, 2, L"Show Window");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, 3, L"Exit");

    // Apply visual styling to context menu
    SetMenuDefaultItem(hMenu, 2, FALSE);

    // Required for correct context menu behavior
    SetForegroundWindow(hwnd);

    // Show menu with modern flags
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_VERPOSANIMATION | TPM_VERNEGANIMATION;
    UINT cmd = TrackPopupMenu(hMenu, flags, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0); // Required for proper cleanup

    switch (cmd) {
    case 1:
        try {
            ToggleFilterVisibility();
        }
        catch (...) {
            // Safely handle any exceptions
            MessageBox(hwnd, L"Error toggling filter visibility.", APP_NAME, MB_ICONERROR);
        }
        break;
    case 2:
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        break;
    case 3:
        // Properly save settings before exit
        SaveSettings();
        DestroyWindow(hwnd);
        break;
    }

    DestroyMenu(hMenu);
}

// Update the tray icon state
void UpdateTrayIcon(bool enabled) {
    // Load the appropriate icon based on filter state
    HICON hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(enabled ? ID_ICON_ACTIVE : ID_ICON));

    // Update icon and tooltip text
    g_notifyIconData.hIcon = hIcon;
    wcscpy_s(g_notifyIconData.szTip, enabled ? L"Privacy Filter: Enabled" : L"Privacy Filter: Disabled");

    if (g_notifyIconData.cbSize >= sizeof(NOTIFYICONDATA)) {
        g_notifyIconData.uFlags |= NIF_INFO;
        g_notifyIconData.dwInfoFlags = NIIF_USER | NIIF_NOSOUND;
        wcscpy_s(g_notifyIconData.szInfoTitle, L"Privacy Filter");
        wcscpy_s(g_notifyIconData.szInfo, enabled ?
            L"Privacy filter has been enabled" :
            L"Privacy filter has been disabled");

        // Show notification briefly and then remove it
        Shell_NotifyIcon(NIM_MODIFY, &g_notifyIconData);

        // Clear notification after display
        g_notifyIconData.szInfo[0] = L'\0';
    }

    // Update the icon
    Shell_NotifyIcon(NIM_MODIFY, &g_notifyIconData);

    // Clean up the icon resource
    DestroyIcon(hIcon);
}

VOID CALLBACK AnimationTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    // Safe increment with bounds checking
    if (g_currentAnimationStep < ANIMATION_STEPS) {
        g_currentAnimationStep++;
    }

    // Check window validity
    if (!IsWindow(hwnd)) {
        if (g_animationTimerId != 0) {
            KillTimer(NULL, g_animationTimerId);
            g_animationTimerId = 0;
        }
        g_animatingFilter = false;
        return;
    }

    // Check if animation has completed
    if (g_currentAnimationStep >= ANIMATION_STEPS) {
        // Stop timer and mark animation as complete
        if (g_animationTimerId != 0) {
            KillTimer(hwnd, g_animationTimerId);
            g_animationTimerId = 0;
        }

        // Finalize state if filter is being disabled
        if (!g_filterEnabled) {
            if (IsWindow(g_hFilterWindow)) {
                // Hide window after fade-out completes
                ShowWindow(g_hFilterWindow, SW_HIDE);
            }

            if (IsWindow(g_hEnableBtn)) {
                SendMessage(g_hEnableBtn, WM_SETTEXT, 0, (LPARAM)L"Enable Filter");
            }

            UpdateTrayIcon(false);
        }
        else {
            // Force a final redraw to ensure complete visibility
            UpdateFilterWindow(true);
        }

        // Mark animation as complete
        g_animatingFilter = false;
        return;
    }

    try {
        // Update the filter window during animation - force redraw each frame
        if (IsWindow(g_hFilterWindow)) {
            UpdateFilterWindow(true);
        }
    }
    catch (...) {
        // If an exception occurs, stop the animation
        if (g_animationTimerId != 0) {
            KillTimer(hwnd, g_animationTimerId);
            g_animationTimerId = 0;
        }
        g_animatingFilter = false;
    }
}

// Draw a gradient background for dark mode
void DrawGradientBackground(HDC hdc, RECT& rect, bool darkMode) {
    if (!darkMode) return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeHighQuality);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);

    // Create a more visually appealing gradient with better contrast for controls
    LinearGradientBrush brush(
        Point(rect.left, rect.top),
        Point(rect.right, rect.bottom),
        Color(255, 10, 40, 60),  // Slightly lighter deep blue
        Color(255, 5, 25, 35)    // Darker deep blue but still with contrast
    );

    // Fill the background
    graphics.FillRectangle(&brush,
        static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.top),
        static_cast<Gdiplus::REAL>(rect.right - rect.left),
        static_cast<Gdiplus::REAL>(rect.bottom - rect.top)
    );

    // Clean up previous brushes
    if (g_themeBrush) DeleteObject(g_themeBrush);
    if (g_frameBrush) DeleteObject(g_frameBrush);

    // Create new themed brushes
    g_themeBrush = CreateSolidBrush(RGB(10, 40, 60));
    g_frameBrush = CreateSolidBrush(RGB(5, 25, 35));
}

// Draw a gradient background for light mode
void DrawGradientBackground2(HDC hdc, RECT& rect, bool lightMode) {
    if (!lightMode) return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeHighQuality);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);

    // Create a more visually appealing gradient for light mode
    LinearGradientBrush brush(
        Point(rect.left, rect.top),
        Point(rect.right, rect.bottom),
        Color(255, 119, 201, 197),  // Teal green
        Color(255, 176, 247, 244)   // Light teal
    );

    // Fill the background
    graphics.FillRectangle(&brush,
        static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.top),
        static_cast<Gdiplus::REAL>(rect.right - rect.left),
        static_cast<Gdiplus::REAL>(rect.bottom - rect.top)
    );

    // Clean up previous brushes
    if (g_themeBrush) DeleteObject(g_themeBrush);
    if (g_frameBrush) DeleteObject(g_frameBrush);

    // Create new themed brushes
    g_themeBrush = CreateSolidBrush(RGB(119, 201, 197));
    g_frameBrush = CreateSolidBrush(RGB(176, 247, 244));
}

// Entry point with all necessary headers
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    // Store instance handle and initialize GDI+
    g_hInstance = hInstance;

    // Initialize GDI+ with specific settings for better rendering
    GdiplusStartupInput gdiplusStartupInput;
    gdiplusStartupInput.GdiplusVersion = 1;
    gdiplusStartupInput.SuppressBackgroundThread = FALSE;
    gdiplusStartupInput.SuppressExternalCodecs = FALSE;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    // Initialize common controls with extended styles
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX),
                                ICC_WIN95_CLASSES |
                                ICC_BAR_CLASSES |
                                ICC_STANDARD_CLASSES |
                                ICC_USEREX_CLASSES };
    InitCommonControlsEx(&icc);

    // Register custom taskbar message
    WM_TASKBAR_ICON = RegisterWindowMessage(L"TaskbarIcon");

    // Load application settings
    LoadSettings();

    // Create application windows
    CreateMainWindow();
    CreateFilterWindow();

    // Initial UI setup
    if (g_hMainWnd) {
        UpdateWindow(g_hMainWnd);
        SetForegroundWindow(g_hMainWnd);

        // Register global hotkey - with proper error handling
        TryRegisterHotkey(g_hMainWnd, g_settings.modifiers, g_settings.toggleHotkey);
        // Show main window according to settings

        if (g_settings.startMinimized) {
            ShowWindow(g_hMainWnd, SW_HIDE);
        }
        else {
            ShowWindow(g_hMainWnd, nCmdShow);
        }

        // Initialize filter state based on settings
        if (g_settings.enableOnStartup) {
            g_filterEnabled = true;
            if (g_hFilterWindow) {
                ShowWindow(g_hFilterWindow, SW_SHOW);
                SetWindowPos(g_hFilterWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            if (g_hEnableBtn) {
                SendMessage(g_hEnableBtn, WM_SETTEXT, 0, (LPARAM)L"Disable Filter");
            }
            UpdateTrayIcon(true);
            UpdateFilterWindow(true);
        }

// Main message loop with improved error handling
        MSG msg = {};
        while (GetMessage(&msg, nullptr, 0, 0)) {
            try {
                // Translate and dispatch messages
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            catch (...) {
                // Log or handle any exceptions in the message loop
                MessageBox(NULL, L"An error occurred processing a message.", APP_NAME, MB_ICONERROR);
            }
        }
    }

    // Cleanup resources
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}

// TitleBarButton::Draw implementation
void TitleBarButton::Draw(HDC hdc) const {
    if (!hdc) return;

    COLORREF normalColor = RGB(28, 33, 28);
    COLORREF hoverColor;

    switch (m_type) {
    case Minimize: hoverColor = RGB(212, 212, 38); break;  // Yellow
    case Maximize: hoverColor = RGB(29, 196, 29); break;   // Green
    case Close: hoverColor = RGB(191, 25, 25); break;      // Red
    }

    COLORREF currentColor = normalColor;
    if (m_hoverState > 0) {

        int r = GetRValue(normalColor) + (GetRValue(hoverColor) - GetRValue(normalColor)) * m_hoverState / 10;
        int g = GetGValue(normalColor) + (GetGValue(hoverColor) - GetGValue(normalColor)) * m_hoverState / 10;
        int b = GetBValue(normalColor) + (GetBValue(hoverColor) - GetBValue(normalColor)) * m_hoverState / 10;

        // Ensure values are within the valid range
        r = min(max(r, 0), 255);
        g = min(max(g, 0), 255);
        b = min(max(b, 0), 255);

        currentColor = RGB(r, g, b);
    }

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush brush(Color(255, GetRValue(currentColor),
        GetGValue(currentColor), GetBValue(currentColor)));
    int width = m_rect.right - m_rect.left;
    int height = m_rect.bottom - m_rect.top;
    graphics.FillEllipse(&brush, Rect(m_rect.left, m_rect.top, width, height));
}

// TitleBarButton::Contains implementation
bool TitleBarButton::Contains(int x, int y) const {
    int centerX = (m_rect.left + m_rect.right) / 2;
    int centerY = (m_rect.top + m_rect.bottom) / 2;
    int radius = (m_rect.right - m_rect.left) / 2;
    return (x - centerX) * (x - centerX) + (y - centerY) * (y - centerY) <= radius * radius;
}

// TitleBarButton::UpdateHoverState implementation
bool TitleBarButton::UpdateHoverState() {
    int target = m_isHovering ? 10 : 0;
    if (m_hoverState == target) return false;

    if (m_isHovering) {
        m_hoverState = min(m_hoverState + 2, 10);
    }
    else {
        m_hoverState = max(m_hoverState - 2, 0);
    }
    return true;
}

// TitleBarButton::SetHover implementation
bool TitleBarButton::SetHover(bool hover) {
    if (m_isHovering == hover) {
        return false;  // No change, so return false
    }
    m_isHovering = hover;
    return true;
}