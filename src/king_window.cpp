#include "king_window.h"

#include <cstdio>

namespace king
{

static std::wstring MakeUniqueClassName()
{
    wchar_t buf[64]{};
    std::swprintf(buf, 64, L"KingWindowClass_%lu", GetCurrentProcessId());
    return std::wstring(buf);
}

Window::~Window()
{
    if (mHwnd)
    {
        DestroyWindow(mHwnd);
        mHwnd = nullptr;
    }

    if (!mClassName.empty() && mInstance)
    {
        UnregisterClassW(mClassName.c_str(), mInstance);
    }
}

void Window::UpdateClientSize()
{
    if (!mHwnd)
        return;

    RECT r{};
    GetClientRect(mHwnd, &r);
    mClientWidth = (uint32_t)(r.right - r.left);
    mClientHeight = (uint32_t)(r.bottom - r.top);
}

void Window::PushEvent(const Event& e)
{
    mEventQueue.push_back(e);
}

bool Window::Create(HINSTANCE hInstance, const Desc& desc)
{
    mInstance = hInstance;
    mClassName = MakeUniqueClassName();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::WndProc;
    wc.hInstance = mInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = mClassName.c_str();

    wc.hIcon = desc.iconBig ? desc.iconBig : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = desc.iconSmall ? desc.iconSmall : LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
        return false;

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!desc.resizable)
    {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }

    RECT wr{ 0, 0, (LONG)desc.width, (LONG)desc.height };
    AdjustWindowRect(&wr, style, FALSE);

    mHwnd = CreateWindowExW(
        0,
        mClassName.c_str(),
        desc.title.c_str(),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        mInstance,
        this
    );

    if (!mHwnd)
        return false;

    SetIcon(wc.hIcon, wc.hIconSm);
    UpdateClientSize();
    return true;
}

void Window::Show(int nCmdShow)
{
    if (!mHwnd)
        return;

    ShowWindow(mHwnd, nCmdShow);
    UpdateWindow(mHwnd);
}

bool Window::PumpMessages()
{
    if (mShouldQuit)
        return false;

    // If the window has been destroyed, do not keep the game loop running.
    if (!mHwnd)
        return false;

    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            mShouldQuit = true;
            return false;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return !mShouldQuit;
}

bool Window::PollEvent(Event& outEvent)
{
    if (mEventQueue.empty())
        return false;

    outEvent = mEventQueue.front();
    mEventQueue.erase(mEventQueue.begin());
    return true;
}

void Window::RequestQuit(int exitCode)
{
    if (mShouldQuit)
        return;

    mShouldQuit = true;
    PostQuitMessage(exitCode);
}

void Window::SetTitle(std::wstring_view title)
{
    if (!mHwnd)
        return;

    SetWindowTextW(mHwnd, std::wstring(title).c_str());
}

void Window::SetIcon(HICON bigIcon, HICON smallIcon)
{
    if (!mHwnd)
        return;

    if (bigIcon)
        SendMessageW(mHwnd, WM_SETICON, ICON_BIG, (LPARAM)bigIcon);
    if (smallIcon)
        SendMessageW(mHwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Window* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto* cs = (CREATESTRUCTW*)lParam;
        self = (Window*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->mHwnd = hwnd;
    }
    else
    {
        self = (Window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }

    if (self)
        return self->HandleMessage(msg, wParam, lParam);

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
    {
        if (wParam == SIZE_MINIMIZED)
            return 0;

        UpdateClientSize();
        if (mClientWidth > 0 && mClientHeight > 0)
        {
            Event e{};
            e.type = EventType::Resize;
            e.width = mClientWidth;
            e.height = mClientHeight;
            PushEvent(e);
        }
        return 0;
    }

    case WM_CLOSE:
    {
        // Make the close (X) button *always* quit the app.
        Event e{};
        e.type = EventType::CloseRequested;
        PushEvent(e);

        RequestQuit(0);
        DestroyWindow(mHwnd);
        return 0;
    }

    case WM_DESTROY:
        // Common place to signal shutdown.
        RequestQuit(0);
        return 0;

    case WM_NCDESTROY:
        // Final destruction point. Ensure our handle is cleared and the app quits.
        mHwnd = nullptr;
        RequestQuit(0);
        return 0;

    default:
        return DefWindowProcW(mHwnd, msg, wParam, lParam);
    }
}

} // namespace king
