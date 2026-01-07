#include "thread_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace king
{

static uint32_t ClampToMax(uint32_t v, uint32_t maxThreads)
{
    if (maxThreads == 0)
        return v;
    return (v > maxThreads) ? maxThreads : v;
}

static std::string Trim(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b]))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

static void ApplyCfgKeyValue(ThreadConfig& cfg, const std::string& keyRaw, const std::string& valRaw)
{
    const std::string key = Trim(keyRaw);
    const std::string val = Trim(valRaw);
    if (key.empty() || val.empty())
        return;

    char* end = nullptr;
    const unsigned long v = std::strtoul(val.c_str(), &end, 10);
    (void)end;
    const uint32_t u = (uint32_t)v;

    if (key == "threads_max") cfg.maxThreads = u;
    else if (key == "threads_ecs") cfg.ecsWorkerThreads = u;
    else if (key == "threads_render_prepare") cfg.renderPrepareWorkerThreads = u;
    else if (key == "threads_render_shadows") cfg.renderShadowRecordThreads = u;
    else if (key == "threads_render_deferred_contexts") cfg.renderDeferredContexts = u;
}

static void LoadFromConfigFile(ThreadConfig& cfg)
{
    // Minimal config format: key=value, one per line.
    // Comments: lines starting with '#' or ';'.

    auto ParseFile = [&](const char* path) -> bool
    {
        std::ifstream f(path);
        if (!f.is_open())
            return false;

        std::string line;
        while (std::getline(f, line))
        {
            std::string t = Trim(line);
            if (t.empty())
                continue;
            if (t[0] == '#' || t[0] == ';')
                continue;

            const size_t eq = t.find('=');
            if (eq == std::string::npos)
                continue;
            ApplyCfgKeyValue(cfg, t.substr(0, eq), t.substr(eq + 1));
        }

        return true;
    };

#ifdef _WIN32
    // Prefer config next to the executable (more robust than relying on CWD).
    // Example: build\Debug\thread_config.cfg
    {
        std::vector<wchar_t> buf;
        buf.resize(32768);
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n > 0 && n < buf.size())
        {
            // Strip filename
            size_t end = n;
            while (end > 0)
            {
                wchar_t c = buf[end - 1];
                if (c == L'\\' || c == L'/')
                    break;
                --end;
            }

            std::wstring dir(buf.data(), buf.data() + end);
            std::wstring cfgPath = dir + L"thread_config.cfg";

            // Convert to UTF-8 for std::ifstream path.
            int needed = WideCharToMultiByte(CP_UTF8, 0, cfgPath.c_str(), (int)cfgPath.size(), nullptr, 0, nullptr, nullptr);
            if (needed > 0)
            {
                std::string u8;
                u8.resize((size_t)needed);
                WideCharToMultiByte(CP_UTF8, 0, cfgPath.c_str(), (int)cfgPath.size(), u8.data(), needed, nullptr, nullptr);
                if (ParseFile(u8.c_str()))
                    return;
            }
        }
    }
#endif

    // Fall back to current working directory.
    (void)ParseFile("thread_config.cfg");
}

#ifdef _WIN32
static uint32_t EnvUIntW(const wchar_t* name, uint32_t defaultValue)
{
    wchar_t buf[64]{};
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0)
        return defaultValue;

    wchar_t* end = nullptr;
    unsigned long v = wcstoul(buf, &end, 10);
    (void)end;
    return (uint32_t)v;
}

static bool EnvFlagW(const wchar_t* name)
{
    wchar_t buf[8]{};
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0)
        return false;
    return (buf[0] == L'1' || buf[0] == L't' || buf[0] == L'T' || buf[0] == L'y' || buf[0] == L'Y');
}
#else
static uint32_t EnvUIntW(const wchar_t*, uint32_t defaultValue)
{
    return defaultValue;
}

static bool EnvFlagW(const wchar_t*)
{
    return false;
}
#endif

ThreadConfig LoadThreadConfig()
{
    ThreadConfig cfg{};

    // Optional: disable the thread_config system entirely (baseline/diagnostics).
    // When disabled, we do NOT read thread_config.cfg and we do NOT apply other KING_THREADS_* overrides.
    if (EnvFlagW(L"KING_DISABLE_THREAD_CONFIG"))
        return cfg;

    // 1) File (persistent)
    LoadFromConfigFile(cfg);

    // 2) Environment (override)
    cfg.maxThreads = EnvUIntW(L"KING_THREADS_MAX", cfg.maxThreads);

    cfg.ecsWorkerThreads = EnvUIntW(L"KING_THREADS_ECS", cfg.ecsWorkerThreads);
    cfg.renderPrepareWorkerThreads = EnvUIntW(L"KING_THREADS_RENDER_PREPARE", cfg.renderPrepareWorkerThreads);
    cfg.renderShadowRecordThreads = EnvUIntW(L"KING_THREADS_RENDER_SHADOWS", cfg.renderShadowRecordThreads);
    cfg.renderDeferredContexts = EnvUIntW(L"KING_THREADS_RENDER_DEFERRED_CONTEXTS", cfg.renderDeferredContexts);

    cfg.ecsWorkerThreads = ClampToMax(cfg.ecsWorkerThreads, cfg.maxThreads);
    cfg.renderPrepareWorkerThreads = ClampToMax(cfg.renderPrepareWorkerThreads, cfg.maxThreads);
    cfg.renderShadowRecordThreads = ClampToMax(cfg.renderShadowRecordThreads, cfg.maxThreads);
    cfg.renderDeferredContexts = ClampToMax(cfg.renderDeferredContexts, cfg.maxThreads);

    return cfg;
}

const ThreadConfig& GetThreadConfig()
{
    static ThreadConfig cfg = LoadThreadConfig();
    return cfg;
}

} // namespace king
