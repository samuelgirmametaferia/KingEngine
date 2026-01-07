#include "perf_analyzer.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <cstdarg>
#endif

namespace king::perf
{

#if defined(_WIN32)
static bool StdoutIsConsole(HANDLE& outHandle)
{
    outHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!outHandle || outHandle == INVALID_HANDLE_VALUE)
        return false;

    DWORD mode = 0;
    if (!GetConsoleMode(outHandle, &mode))
        return false;
    return true;
}

static void ClearConsole(HANDLE h)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!GetConsoleScreenBufferInfo(h, &csbi))
        return;

    const DWORD cellCount = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    DWORD written = 0;
    COORD home{ 0, 0 };
    FillConsoleOutputCharacterA(h, ' ', cellCount, home, &written);
    FillConsoleOutputAttribute(h, csbi.wAttributes, cellCount, home, &written);
    SetConsoleCursorPosition(h, home);
}

static void ConsoleHome(HANDLE h)
{
    COORD home{ 0, 0 };
    SetConsoleCursorPosition(h, home);
}
#endif

static void PrintLine(bool toStdout, const char* text)
{
    if (!text)
        return;

    if (toStdout)
        std::printf("%s", text);

#if defined(_WIN32)
    // In GUI subsystem builds, stdout may not be visible. Mirror output to the debugger.
    if (text[0] != '\x1b')
        OutputDebugStringA(text);
#endif
}

#if defined(_WIN32)
static bool EnsureVtConsole()
{
    static bool sInit = false;
    static bool sOk = false;
    if (sInit)
        return sOk;
    sInit = true;

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr)
        return false;

    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode))
        return false;

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(h, mode))
        return false;

    sOk = true;
    return true;
}

static void ConsoleOverlayBegin(bool toStdout)
{
    if (!toStdout)
        return;
    if (!EnsureVtConsole())
        return;

    static bool sClearedOnce = false;
    if (!sClearedOnce)
    {
        // Clear screen once to get rid of prior scrolling spam.
        PrintLine(true, "\x1b[2J");
        sClearedOnce = true;
    }

    // Move cursor to home; we'll clear lines as we overwrite them.
    PrintLine(true, "\x1b[H");
}

static void ConsoleOverlayPrintfLine(bool toStdout, const char* fmt, ...)
{
    if (!fmt)
        return;

    char buf[1024];
    buf[0] = '\0';

    va_list args;
    va_start(args, fmt);
    (void)std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (toStdout && EnsureVtConsole())
    {
        // Clear current line, print, newline.
        PrintLine(true, "\x1b[2K");
        PrintLine(true, buf);
        PrintLine(true, "\n");
    }
    else
    {
        PrintLine(toStdout, buf);
        PrintLine(toStdout, "\n");
    }
}

#endif

static double MsSince(const std::chrono::steady_clock::time_point& start,
                       const std::chrono::steady_clock::time_point& end)
{
    using namespace std::chrono;
    return duration<double, std::milli>(end - start).count();
}

PerfAnalyzer::Sample* PerfAnalyzer::FindOrAdd(std::vector<Sample>& samples, const char* name)
{
    for (auto& s : samples)
    {
        if (s.name == name)
            return &s;
        if (s.name && name && std::strcmp(s.name, name) == 0)
            return &s;
    }

    Sample ns{};
    ns.name = name;
    samples.push_back(ns);
    return &samples.back();
}

void PerfAnalyzer::BeginFrame()
{
    if (!mSettings.enabled)
        return;

    // Keep capacity to avoid churn; just reset values.
    for (auto& s : mSamples)
    {
        s.cpuMs = 0.0;
        s.gpuMs = -1.0;
    }
}

void PerfAnalyzer::SetFps(double fps)
{
    if (fps <= 1e-6)
        return;
    mHaveFps = true;
    mLastFps = fps;
    mLastMsPerFrame = 1000.0 / fps;
}

void PerfAnalyzer::EndFrame()
{
    if (!mSettings.enabled)
        return;

    mFrameIndex++;

    if (!mSettings.printToStdout)
        return;

    const bool shouldPrint = (mSettings.printEveryNFrames > 0) && ((mFrameIndex % mSettings.printEveryNFrames) == 0);
    if (!shouldPrint)
        return;

#if defined(_WIN32)
    HANDLE h = nullptr;
    const bool canOverwrite = mSettings.printToStdout && mSettings.overwriteConsole && StdoutIsConsole(h);
    if (canOverwrite)
    {
        if (!mConsolePrepared)
        {
            if (mSettings.clearConsoleOnFirstPrint)
                ClearConsole(h);
            mConsolePrepared = true;
        }
        else
        {
            ConsoleHome(h);
        }

        auto findSample = [&](const char* name) -> const Sample*
        {
            for (const auto& s : mSamples)
            {
                if (!s.name)
                    continue;
                if (s.name == name)
                    return &s;
                if (name && std::strcmp(s.name, name) == 0)
                    return &s;
            }
            return nullptr;
        };

        auto printOne = [&](const char* name)
        {
            const Sample* s = findSample(name);
            if (!s)
            {
                std::printf("  %-16s CPU   (n/a)   | GPU   (n/a)\n", name);
                return;
            }
            if (s->gpuMs >= 0.0)
                std::printf("  %-16s CPU %7.3f ms | GPU %7.3f ms\n", s->name, s->cpuMs, s->gpuMs);
            else
                std::printf("  %-16s CPU %7.3f ms | GPU   (n/a)\n", s->name, s->cpuMs);
        };

        if (mHaveFps)
            std::printf("FPS: %.1f (%.2f ms)\n", mLastFps, mLastMsPerFrame);
        std::printf("[Perf] frame=%llu\n", (unsigned long long)mFrameIndex);
        printOne("Frame");
        printOne("ShadowPass");
        printOne("GeometryPass");
        printOne("SSAOPass");
        printOne("TonemapPass");

        mLastPrintedLines = mHaveFps ? 7u : 6u;
        std::fflush(stdout);
        return;
    }
#endif

    // Fallback: regular logging (stdout + debugger mirror).
#if defined(_WIN32)
    ConsoleOverlayBegin(mSettings.printToStdout);
#endif

#if defined(_WIN32)
    if (mHaveFps)
        ConsoleOverlayPrintfLine(mSettings.printToStdout, "FPS: %.1f (%.2f ms)", mLastFps, mLastMsPerFrame);
#else
    if (mHaveFps)
        PrintfLine(mSettings.printToStdout, "FPS: %.1f (%.2f ms)\n", mLastFps, mLastMsPerFrame);
#endif

#if defined(_WIN32)
    ConsoleOverlayPrintfLine(mSettings.printToStdout, "[Perf] frame=%llu", (unsigned long long)mFrameIndex);
#else
    PrintfLine(mSettings.printToStdout, "[Perf] frame=%llu\n", (unsigned long long)mFrameIndex);
#endif
    for (const auto& s : mSamples)
    {
        if (!s.name)
            continue;
        if (s.gpuMs >= 0.0)
        {
#if defined(_WIN32)
            ConsoleOverlayPrintfLine(mSettings.printToStdout, "  %-16s CPU %7.3f ms | GPU %7.3f ms", s.name, s.cpuMs, s.gpuMs);
#else
            PrintfLine(mSettings.printToStdout, "  %-16s CPU %7.3f ms | GPU %7.3f ms\n", s.name, s.cpuMs, s.gpuMs);
#endif
        }
        else
        {
#if defined(_WIN32)
            ConsoleOverlayPrintfLine(mSettings.printToStdout, "  %-16s CPU %7.3f ms", s.name, s.cpuMs);
#else
            PrintfLine(mSettings.printToStdout, "  %-16s CPU %7.3f ms\n", s.name, s.cpuMs);
#endif
        }
    }

#if defined(_WIN32)
    // Clear the remainder of the screen below our overlay.
    if (mSettings.printToStdout && EnsureVtConsole())
        PrintLine(true, "\x1b[J");
#endif

    if (mSettings.printToStdout)
        std::fflush(stdout);
}

void PerfAnalyzer::AddCpuMs(const char* name, double ms)
{
    if (!mSettings.enabled)
        return;
    auto* s = FindOrAdd(mSamples, name);
    s->cpuMs += ms;
}

void PerfAnalyzer::AddGpuMs(const char* name, double ms)
{
    if (!mSettings.enabled)
        return;
    auto* s = FindOrAdd(mSamples, name);
    // Keep the latest reading for the frame.
    s->gpuMs = ms;
}

CpuScope::CpuScope(PerfAnalyzer& perf, const char* name)
    : mPerf(&perf), mName(name), mStart(std::chrono::steady_clock::now())
{
}

CpuScope::~CpuScope()
{
    if (!mPerf || !mPerf->Enabled() || !mName)
        return;

    const auto end = std::chrono::steady_clock::now();
    const double ms = MsSince(mStart, end);
    mPerf->AddCpuMs(mName, ms);
}

} // namespace king::perf
