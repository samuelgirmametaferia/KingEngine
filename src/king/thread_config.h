#pragma once

#include <cstdint>

namespace king
{
struct ThreadConfig
{
    // 0 = run single-threaded for that subsystem.

    // Render: CPU frame prep worker (BuildPreparedFrame off-thread).
    // Current implementation is either 0 (disabled) or 1 (enabled).
    uint32_t renderPrepareWorkerThreads = 1;

    // Render: worker threads used to record shadow cascades.
    // 0/1 = sequential recording.
    uint32_t renderShadowRecordThreads = 0;

    // Render: number of D3D11 deferred contexts to create when deferred recording is enabled.
    // 0 = create the minimal number needed (1).
    uint32_t renderDeferredContexts = 0;

    // ECS: reserved for ECS parallel iteration / system scheduling.
    // (ECS is currently single-threaded, but subsystems can consult this.)
    uint32_t ecsWorkerThreads = 0;

    // Optional global clamp. 0 = no clamp.
    uint32_t maxThreads = 0;
};

// Loads config from thread_config.cfg (prefers next to the executable, then CWD)
// and then applies environment variable overrides (Windows: GetEnvironmentVariableW).
// Variables:
// - KING_THREADS_MAX
// - KING_THREADS_ECS
// - KING_THREADS_RENDER_PREPARE
// - KING_THREADS_RENDER_SHADOWS
// - KING_THREADS_RENDER_DEFERRED_CONTEXTS
ThreadConfig LoadThreadConfig();

// Cached singleton (calls LoadThreadConfig() once on first use).
const ThreadConfig& GetThreadConfig();

} // namespace king
