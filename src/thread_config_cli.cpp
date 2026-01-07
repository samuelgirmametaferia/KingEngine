#include "king/thread_config.h"

#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void ClearScreen()
{
    // Simple & portable enough for Windows terminals.
    std::system("cls");
}

static void SaveConfig(const king::ThreadConfig& cfg)
{
    // Persist next to the executable (engine prefers this location), fall back to CWD.
    std::string path = "thread_config.cfg";

#ifdef _WIN32
    {
        wchar_t exePathW[32768]{};
        DWORD n = GetModuleFileNameW(nullptr, exePathW, (DWORD)(sizeof(exePathW) / sizeof(exePathW[0])));
        if (n > 0 && n < (DWORD)(sizeof(exePathW) / sizeof(exePathW[0])))
        {
            // Strip filename
            size_t end = n;
            while (end > 0)
            {
                wchar_t c = exePathW[end - 1];
                if (c == L'\\' || c == L'/')
                    break;
                --end;
            }
            std::wstring dir(exePathW, exePathW + end);
            std::wstring cfgW = dir + L"thread_config.cfg";

            int needed = WideCharToMultiByte(CP_UTF8, 0, cfgW.c_str(), (int)cfgW.size(), nullptr, 0, nullptr, nullptr);
            if (needed > 0)
            {
                std::string u8;
                u8.resize((size_t)needed);
                WideCharToMultiByte(CP_UTF8, 0, cfgW.c_str(), (int)cfgW.size(), u8.data(), needed, nullptr, nullptr);
                path = u8;
            }
        }
    }
#endif

    std::ofstream f(path.c_str(), std::ios::trunc);
    if (!f.is_open())
    {
        // Fallback to CWD
        std::ofstream f2("thread_config.cfg", std::ios::trunc);
        if (!f2.is_open())
            return;
        f.swap(f2);
    }

    f << "# King thread config\n";
    f << "# Edited by ThreadConfigCLI\n";
    f << "threads_max=" << cfg.maxThreads << "\n";
    f << "threads_ecs=" << cfg.ecsWorkerThreads << "\n";
    f << "threads_render_prepare=" << cfg.renderPrepareWorkerThreads << "\n";
    f << "threads_render_shadows=" << cfg.renderShadowRecordThreads << "\n";
    f << "threads_render_deferred_contexts=" << cfg.renderDeferredContexts << "\n";
}

static uint32_t ClampU32(int v)
{
    if (v < 0) return 0u;
    if (v > 64) return 64u;
    return (uint32_t)v;
}

int main()
{
    // Start from the currently loaded config (file + env overrides).
    // NOTE: This CLI persists to file; env vars still override at runtime.
    king::ThreadConfig cfg = king::LoadThreadConfig();

    enum Field
    {
        MaxThreads = 0,
        EcsThreads,
        RenderPrepareThreads,
        RenderShadowThreads,
        RenderDeferredContexts,
        FieldCount
    };

    int selected = 0;

    for (;;)
    {
        ClearScreen();

        std::printf("King Thread Config CLI\n\n");
        std::printf("Up/Down: select   Left/Right: change   Enter: save+exit   Esc: exit\n\n");

        auto PrintRow = [&](int idx, const char* name, uint32_t value)
        {
            const char* cursor = (idx == selected) ? ">" : " ";
            std::printf("%s %-32s %u\n", cursor, name, value);
        };

        PrintRow(MaxThreads, "maxThreads (0 = no clamp)", cfg.maxThreads);
        PrintRow(EcsThreads, "ecsWorkerThreads", cfg.ecsWorkerThreads);
        PrintRow(RenderPrepareThreads, "renderPrepareWorkerThreads", cfg.renderPrepareWorkerThreads);
        PrintRow(RenderShadowThreads, "renderShadowRecordThreads", cfg.renderShadowRecordThreads);
        PrintRow(RenderDeferredContexts, "renderDeferredContexts", cfg.renderDeferredContexts);

        int ch = _getch();
        if (ch == 27) // ESC
            return 0;

        if (ch == 13) // Enter
        {
            SaveConfig(cfg);
            return 0;
        }

        if (ch == 0 || ch == 224)
        {
            const int key = _getch();
            switch (key)
            {
            case 72: // Up
                selected = (selected <= 0) ? (FieldCount - 1) : (selected - 1);
                break;
            case 80: // Down
                selected = (selected >= (FieldCount - 1)) ? 0 : (selected + 1);
                break;
            case 75: // Left
            case 77: // Right
            {
                const int delta = (key == 77) ? +1 : -1;

                auto ApplyDelta = [&](uint32_t& v)
                {
                    v = ClampU32((int)v + delta);
                };

                switch ((Field)selected)
                {
                case MaxThreads: ApplyDelta(cfg.maxThreads); break;
                case EcsThreads: ApplyDelta(cfg.ecsWorkerThreads); break;
                case RenderPrepareThreads: ApplyDelta(cfg.renderPrepareWorkerThreads); break;
                case RenderShadowThreads: ApplyDelta(cfg.renderShadowRecordThreads); break;
                case RenderDeferredContexts: ApplyDelta(cfg.renderDeferredContexts); break;
                default: break;
                }

                // Keep values within clamp immediately (except max itself).
                if (cfg.maxThreads != 0)
                {
                    if (cfg.ecsWorkerThreads > cfg.maxThreads) cfg.ecsWorkerThreads = cfg.maxThreads;
                    if (cfg.renderPrepareWorkerThreads > cfg.maxThreads) cfg.renderPrepareWorkerThreads = cfg.maxThreads;
                    if (cfg.renderShadowRecordThreads > cfg.maxThreads) cfg.renderShadowRecordThreads = cfg.maxThreads;
                    if (cfg.renderDeferredContexts > cfg.maxThreads) cfg.renderDeferredContexts = cfg.maxThreads;
                }
            }
            break;
            default:
                break;
            }
        }
    }
}
