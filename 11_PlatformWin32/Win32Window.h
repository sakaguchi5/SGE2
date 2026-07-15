#pragma once

#include "../00_Base/Result.h"
#include "../09_PackageRuntime/PackageRuntime.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace sge::platform
{
class Win32Window final : public runtime::ISurfaceHost
{
public:
    [[nodiscard]] static base::Result<std::unique_ptr<Win32Window>, std::string> Create(
        const wchar_t* title,
        std::uint32_t renderWidth,
        std::uint32_t renderHeight,
        std::uint32_t telemetryPanelWidth = 0);

    ~Win32Window() override;
    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    [[nodiscard]] bool PumpMessages();
    void Show(int command = SW_SHOWDEFAULT);
    void SetTelemetryText(std::wstring text);

    [[nodiscard]] void* NativeWindowHandle() const noexcept override
    {
        return renderHwnd_ ? renderHwnd_ : hwnd_;
    }
    [[nodiscard]] std::uint32_t ClientWidth() const noexcept override { return renderWidth_; }
    [[nodiscard]] std::uint32_t ClientHeight() const noexcept override { return renderHeight_; }
    [[nodiscard]] bool HasTelemetryPanel() const noexcept { return telemetryHwnd_ != nullptr; }

private:
    Win32Window() = default;
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TelemetryWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void LayoutChildren(std::uint32_t totalWidth, std::uint32_t totalHeight);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND renderHwnd_ = nullptr;
    HWND telemetryHwnd_ = nullptr;
    HFONT telemetryFont_ = nullptr;
    std::uint32_t renderWidth_ = 0;
    std::uint32_t renderHeight_ = 0;
    std::uint32_t telemetryPanelWidth_ = 0;
    std::wstring telemetryText_;
    bool running_ = true;
};
}
