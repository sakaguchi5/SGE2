#include "Win32Window.h"

#include <utility>

namespace sge::platform
{
namespace
{
constexpr wchar_t WindowClassName[] = L"SemanticGpuEngine2WindowClass";
constexpr wchar_t RenderClassName[] = L"SemanticGpuEngine2RenderSurfaceClass";
constexpr wchar_t TelemetryClassName[] = L"SemanticGpuEngine2TelemetryPanelClass";
constexpr int TelemetryMargin = 14;
constexpr int TelemetryTextInset = 12;
constexpr COLORREF TelemetryBackground = RGB(18, 22, 29);
constexpr COLORREF TelemetryForeground = RGB(220, 232, 240);
constexpr COLORREF TelemetryBorder = RGB(64, 78, 92);

bool RegisterWindowClasses(HINSTANCE instance, WNDPROC parentProc, WNDPROC telemetryProc)
{
    WNDCLASSEXW parent{};
    parent.cbSize = sizeof(parent);
    parent.style = CS_HREDRAW | CS_VREDRAW;
    parent.lpfnWndProc = parentProc;
    parent.hInstance = instance;
    parent.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    parent.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    parent.lpszClassName = WindowClassName;
    if (!RegisterClassExW(&parent) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW render{};
    render.cbSize = sizeof(render);
    render.style = CS_HREDRAW | CS_VREDRAW;
    render.lpfnWndProc = DefWindowProcW;
    render.hInstance = instance;
    render.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    render.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    render.lpszClassName = RenderClassName;
    if (!RegisterClassExW(&render) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW telemetry{};
    telemetry.cbSize = sizeof(telemetry);
    telemetry.style = 0;
    telemetry.lpfnWndProc = telemetryProc;
    telemetry.hInstance = instance;
    telemetry.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    telemetry.hbrBackground = nullptr;
    telemetry.lpszClassName = TelemetryClassName;
    if (!RegisterClassExW(&telemetry) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    return true;
}
}

base::Result<std::unique_ptr<Win32Window>, std::string> Win32Window::Create(
    const wchar_t* title,
    std::uint32_t renderWidth,
    std::uint32_t renderHeight,
    std::uint32_t telemetryPanelWidth)
{
    if (renderWidth == 0 || renderHeight == 0)
        return base::Result<std::unique_ptr<Win32Window>, std::string>::Failure("Render surface dimensions must be non-zero");
    if (telemetryPanelWidth > 0 && telemetryPanelWidth <= static_cast<std::uint32_t>(TelemetryMargin * 2))
        return base::Result<std::unique_ptr<Win32Window>, std::string>::Failure("Telemetry panel is too narrow");

    auto window = std::unique_ptr<Win32Window>(new Win32Window());
    window->instance_ = GetModuleHandleW(nullptr);
    window->renderWidth_ = renderWidth;
    window->renderHeight_ = renderHeight;
    window->telemetryPanelWidth_ = telemetryPanelWidth;

    if (!RegisterWindowClasses(
            window->instance_,
            &Win32Window::WindowProc,
            &Win32Window::TelemetryWindowProc))
    {
        return base::Result<std::unique_ptr<Win32Window>, std::string>::Failure("RegisterClassExW failed");
    }

    constexpr DWORD WindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const std::uint32_t totalClientWidth = renderWidth + telemetryPanelWidth;
    RECT rect{0, 0, static_cast<LONG>(totalClientWidth), static_cast<LONG>(renderHeight)};
    if (!AdjustWindowRect(&rect, WindowStyle, FALSE))
        return base::Result<std::unique_ptr<Win32Window>, std::string>::Failure("AdjustWindowRect failed");

    window->hwnd_ = CreateWindowExW(
        0, WindowClassName, title, WindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, window->instance_, window.get());
    if (!window->hwnd_)
        return base::Result<std::unique_ptr<Win32Window>, std::string>::Failure("CreateWindowExW failed");

    if (telemetryPanelWidth > 0)
    {
        window->renderHwnd_ = CreateWindowExW(
            0, RenderClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, static_cast<int>(renderWidth), static_cast<int>(renderHeight),
            window->hwnd_, nullptr, window->instance_, nullptr);
        if (!window->renderHwnd_)
            return base::Result<std::unique_ptr<Win32Window>, std::string>::Failure("Create render child window failed");

        window->telemetryFont_ = CreateFontW(
            -17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        window->telemetryText_ = L"Telemetry initializing...";
        window->telemetryHwnd_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, TelemetryClassName, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            static_cast<int>(renderWidth + TelemetryMargin), TelemetryMargin,
            static_cast<int>(telemetryPanelWidth - TelemetryMargin * 2),
            static_cast<int>(renderHeight - TelemetryMargin * 2),
            window->hwnd_, nullptr, window->instance_, window.get());
        if (!window->telemetryHwnd_)
            return base::Result<std::unique_ptr<Win32Window>, std::string>::Failure("Create telemetry panel failed");
    }

    return base::Result<std::unique_ptr<Win32Window>, std::string>::Success(std::move(window));
}

Win32Window::~Win32Window()
{
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        renderHwnd_ = nullptr;
        telemetryHwnd_ = nullptr;
    }
    if (telemetryFont_)
    {
        DeleteObject(telemetryFont_);
        telemetryFont_ = nullptr;
    }
}

void Win32Window::Show(int command)
{
    ShowWindow(hwnd_, command);
    UpdateWindow(hwnd_);
}

void Win32Window::SetTelemetryText(std::wstring text)
{
    telemetryText_ = std::move(text);
    if (telemetryHwnd_)
        InvalidateRect(telemetryHwnd_, nullptr, FALSE);
}

bool Win32Window::PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT) running_ = false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return running_;
}

void Win32Window::LayoutChildren(std::uint32_t totalWidth, std::uint32_t totalHeight)
{
    if (telemetryPanelWidth_ == 0)
    {
        renderWidth_ = totalWidth;
        renderHeight_ = totalHeight;
        return;
    }
    if (!renderHwnd_ || !telemetryHwnd_)
        return;

    const std::uint32_t panelWidth = totalWidth > telemetryPanelWidth_ ? telemetryPanelWidth_ : totalWidth;
    const std::uint32_t renderWidth = totalWidth - panelWidth;
    renderWidth_ = renderWidth;
    renderHeight_ = totalHeight;

    MoveWindow(renderHwnd_, 0, 0, static_cast<int>(renderWidth), static_cast<int>(totalHeight), TRUE);

    const int panelX = static_cast<int>(renderWidth) + TelemetryMargin;
    const int panelY = TelemetryMargin;
    const int panelClientWidth = static_cast<int>(panelWidth) - TelemetryMargin * 2;
    const int panelClientHeight = static_cast<int>(totalHeight) - TelemetryMargin * 2;
    MoveWindow(
        telemetryHwnd_, panelX, panelY,
        panelClientWidth > 1 ? panelClientWidth : 1,
        panelClientHeight > 1 ? panelClientHeight : 1,
        TRUE);
}

LRESULT CALLBACK Win32Window::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    Win32Window* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Win32Window*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    switch (message)
    {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (self) self->running_ = false;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (self && wParam != SIZE_MINIMIZED)
        {
            self->LayoutChildren(
                static_cast<std::uint32_t>(LOWORD(lParam)),
                static_cast<std::uint32_t>(HIWORD(lParam)));
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK Win32Window::TelemetryWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    Win32Window* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Win32Window*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    switch (message)
    {
    case WM_ERASEBKGND:
        // WM_PAINT always covers the complete client area. Suppressing the
        // default erase removes the visible background-only intermediate frame.
        return 1;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        const HDC targetDc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right > client.left
            ? static_cast<int>(client.right - client.left)
            : 0;
        const int height = client.bottom > client.top
            ? static_cast<int>(client.bottom - client.top)
            : 0;

        const auto drawPanel = [&](HDC dc)
        {
            const HBRUSH backgroundBrush = CreateSolidBrush(TelemetryBackground);
            if (backgroundBrush)
            {
                FillRect(dc, &client, backgroundBrush);
                DeleteObject(backgroundBrush);
            }

            RECT borderRect = client;
            if (borderRect.right > borderRect.left && borderRect.bottom > borderRect.top)
            {
                --borderRect.right;
                --borderRect.bottom;
                const HBRUSH borderBrush = CreateSolidBrush(TelemetryBorder);
                if (borderBrush)
                {
                    FrameRect(dc, &borderRect, borderBrush);
                    DeleteObject(borderBrush);
                }
            }

            SetTextColor(dc, TelemetryForeground);
            SetBkMode(dc, TRANSPARENT);

            const HGDIOBJ font = self && self->telemetryFont_
                ? reinterpret_cast<HGDIOBJ>(self->telemetryFont_)
                : GetStockObject(DEFAULT_GUI_FONT);
            const HGDIOBJ previousFont = font ? SelectObject(dc, font) : nullptr;

            const LONG textRight = client.right > TelemetryTextInset * 2
                ? client.right - TelemetryTextInset
                : TelemetryTextInset;
            const LONG textBottom = client.bottom > TelemetryTextInset * 2
                ? client.bottom - TelemetryTextInset
                : TelemetryTextInset;
            RECT textRect{
                TelemetryTextInset,
                TelemetryTextInset,
                textRight,
                textBottom};
            const wchar_t* text = self ? self->telemetryText_.c_str() : L"";
            DrawTextW(
                dc,
                text,
                -1,
                &textRect,
                DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);

            if (previousFont)
                SelectObject(dc, previousFont);
        };

        if (width > 0 && height > 0)
        {
            const HDC memoryDc = CreateCompatibleDC(targetDc);
            const HBITMAP bitmap = memoryDc
                ? CreateCompatibleBitmap(targetDc, width, height)
                : nullptr;

            if (memoryDc && bitmap)
            {
                const HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
                drawPanel(memoryDc);
                BitBlt(targetDc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);
                SelectObject(memoryDc, previousBitmap);
                DeleteObject(bitmap);
                DeleteDC(memoryDc);
            }
            else
            {
                if (bitmap)
                    DeleteObject(bitmap);
                if (memoryDc)
                    DeleteDC(memoryDc);
                drawPanel(targetDc);
            }
        }

        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}
