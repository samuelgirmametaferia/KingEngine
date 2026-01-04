#pragma once

#include <cstdint>

namespace king
{

// Simple timekeeper for:
// - variable frame delta (clamped)
// - fixed timestep accumulator
// - FPS sampling (once per ~1s)
class Time
{
public:
    Time();

    void Reset();
    void Tick();

    // Fixed-step loop helper.
    // Returns true if a fixed update step should run.
    bool ConsumeFixedStep();

    double TotalSeconds() const { return mTotalSeconds; }
    double FixedTimeSeconds() const { return mFixedTimeSeconds; }
    double DeltaSeconds() const { return mDeltaSeconds; }
    double FixedDeltaSeconds() const { return mFixedDeltaSeconds; }
    double Alpha() const; // interpolation factor [0..1]

    // FPS sampling.
    bool FpsUpdated() const { return mFpsUpdated; }
    double Fps() const { return mFps; }

    // Config
    void SetFixedDeltaSeconds(double dt);
    void SetMaxDeltaSeconds(double dt);

private:
    static int64_t NowTicks();

    bool mInitialized = false;

    int64_t mStartTicks = 0;
    int64_t mLastTicks = 0;

    double mTotalSeconds = 0.0;
    double mFixedTimeSeconds = 0.0;
    double mDeltaSeconds = 0.0;

    double mFixedDeltaSeconds = 1.0 / 60.0;
    double mMaxDeltaSeconds = 0.10;

    double mAccumulatorSeconds = 0.0;

    // FPS sampling
    double mFps = 0.0;
    double mFpsWindowSeconds = 0.0;
    uint32_t mFpsFrameCount = 0;
    bool mFpsUpdated = false;
};

} // namespace king
