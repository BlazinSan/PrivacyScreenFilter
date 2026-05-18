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

            // Update text color based on theme - make sure the contrast is sufficient
            g_textColor = currentTheme == Theme::Dark ? RGB(240, 240, 240) : RGB(0, 0, 0);  // Pure black for light mode

            if (g_the