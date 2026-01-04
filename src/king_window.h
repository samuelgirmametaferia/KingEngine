#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace king
{

class Window
{
public:
    enum class EventType
    {
        None = 0,
        Resize,
        CloseRequested,
    };

    struct Event
    {
        EventType type = EventType::None;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct Desc
    {
        uint32_t width = 1280;
        uint32_t height = 720;
        std::wstring title = L"King";
        bool resizable = true;

        // If null, a default system icon is used.
        HICON iconBig = nullptr;
        HICON iconSmall = nullptr;
    };

    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(HINSTANCE hInstance, const Desc& desc);
    void Show(int nCmdShow);

    // Pumps messages; returns false when the app should quit.
    bool PumpMessages();

    // Poll-style events (friendlier for bindings than callbacks).
    bool PollEvent(Event& outEvent);

    // Immediate quit request (posts WM_QUIT).
    void RequestQuit(int exitCode = 0);

    HWND Handle() const { return mHwnd; }

    void SetTitle(std::wstring_view title);
    void SetIcon(HICON bigIcon, HICON smallIcon);

    uint32_t ClientWidth() const { return mClientWidth; }
    uint32_t ClientHeight() const { return mClientHeight; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void UpdateClientSize();
    void PushEvent(const Event& e);

    HINSTANCE mInstance = nullptr;
    HWND mHwnd = nullptr;
    std::wstring mClassName;

    uint32_t mClientWidth = 0;
    uint32_t mClientHeight = 0;

    bool mShouldQuit = false;
    std::vector<Event> mEventQueue;
};

} // namespace king
