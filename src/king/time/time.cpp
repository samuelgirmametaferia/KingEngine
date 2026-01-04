#include "time.h"

#include <windows.h>

#include <algorithm>

namespace king
{

static double TicksToSeconds(int64_t ticks)
{
    static double invFreq = 0.0;
    if (invFreq == 0.0)
    {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        invFreq = 1.0 / (double)freq.QuadPart;
    }
    return (double)ticks * invFreq;
}

int64_t Time::NowTicks()
{
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return (int64_t)c.QuadPart;
}

Time::Time() = default;

void Time::Reset()
{
    mStartTicks = NowTicks();
    mLastTicks = mStartTicks;

    mInitialized = true;

    mTotalSeconds = 0.0;
    mFixedTimeSeconds = 0.0;
    mDeltaSeconds = 0.0;
    mAccumulatorSeconds = 0.0;

    mFps = 0.0;
    mFpsWindowSeconds = 0.0;
    mFpsFrameCount = 0;
    mFpsUpdated = false;
}

void Time::Tick()
{
    if (!mInitialized)
        Reset();

    mFpsUpdated = false;

    const int64_t nowTicks = NowTicks();
    const int64_t dtTicks = nowTicks - mLastTicks;
    mLastTicks = nowTicks;

    double dt = TicksToSeconds(dtTicks);
    if (dt < 0.0)
        dt = 0.0;
    dt = std::min(dt, mMaxDeltaSeconds);

    mDeltaSeconds = dt;
    mTotalSeconds += dt;

    mAccumulatorSeconds += dt;

    // FPS sampling: compute over ~1 second windows.
    mFpsWindowSeconds += dt;
    mFpsFrameCount++;
    if (mFpsWindowSeconds >= 1.0)
    {
        mFps = (double)mFpsFrameCount / mFpsWindowSeconds;
        mFpsWindowSeconds = 0.0;
        mFpsFrameCount = 0;
        mFpsUpdated = true;
    }
}

bool Time::ConsumeFixedStep()
{
    if (mFixedDeltaSeconds <= 0.0)
        return false;

    if (mAccumulatorSeconds + 1e-12 < mFixedDeltaSeconds)
        return false;

    mAccumulatorSeconds -= mFixedDeltaSeconds;
    mFixedTimeSeconds += mFixedDeltaSeconds;
    return true;
}

double Time::Alpha() const
{
    if (mFixedDeltaSeconds <= 0.0)
        return 0.0;
    double a = mAccumulatorSeconds / mFixedDeltaSeconds;
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return a;
}

void Time::SetFixedDeltaSeconds(double dt)
{
    mFixedDeltaSeconds = std::max(0.0, dt);
}

void Time::SetMaxDeltaSeconds(double dt)
{
    mMaxDeltaSeconds = std::max(0.0, dt);
}

} // namespace king
