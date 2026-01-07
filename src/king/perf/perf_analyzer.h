#pragma once

#include <cstdint>
#include <chrono>
#include <vector>

namespace king::perf
{

class PerfAnalyzer
{
public:
    struct Settings
    {
        bool enabled = true;
        bool printToStdout = true;

        // If stdout is a real console, overwrite the same block each print.
        // This keeps the CLI clean and avoids flicker from scrolling output.
        bool overwriteConsole = true;
        bool clearConsoleOnFirstPrint = true;

        // Print once per N frames (fallback if no stable wall clock is desired).
        uint32_t printEveryNFrames = 60;
    };

    struct Sample
    {
        const char* name = nullptr;
        double cpuMs = 0.0;
        double gpuMs = -1.0; // <0 means unavailable
    };

    explicit PerfAnalyzer(Settings s = {}) : mSettings(s) {}

    void SetEnabled(bool enabled) { mSettings.enabled = enabled; }
    bool Enabled() const { return mSettings.enabled; }

    // Output control (collection remains enabled).
    void SetPrintToStdout(bool enabled) { mSettings.printToStdout = enabled; }
    void SetPrintEveryNFrames(uint32_t n) { mSettings.printEveryNFrames = n; }
    void SetOverwriteConsole(bool enabled) { mSettings.overwriteConsole = enabled; }

    void BeginFrame();
    void EndFrame();

    // Optional: if provided, FPS is printed as part of the overlay.
    void SetFps(double fps);

    void AddCpuMs(const char* name, double ms);
    void AddGpuMs(const char* name, double ms);

    const std::vector<Sample>& Samples() const { return mSamples; }

private:
    static Sample* FindOrAdd(std::vector<Sample>& samples, const char* name);

private:
    Settings mSettings{};
    std::vector<Sample> mSamples;
    uint64_t mFrameIndex = 0;

    // Console presentation state (best-effort; only used when stdout is a console).
    bool mConsolePrepared = false;
    uint32_t mLastPrintedLines = 0;

    bool mHaveFps = false;
    double mLastFps = 0.0;
    double mLastMsPerFrame = 0.0;
};

class CpuScope
{
public:
    CpuScope(PerfAnalyzer& perf, const char* name);
    ~CpuScope();

    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;

private:
    PerfAnalyzer* mPerf = nullptr;
    const char* mName = nullptr;
    std::chrono::steady_clock::time_point mStart;
};

} // namespace king::perf
