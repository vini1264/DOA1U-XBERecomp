// "RECOMP SETTINGS" in the OPTION MODE screen, between RECORDS and EXIT, drawn with the game's own text routines.
//
// The game, as read from Doa1.xbe (guest addresses):
//  * The whole game runs as coroutines on XAPI fibers (see fibers.cpp); 0x520A0(n) waits n frames by yielding.
//  * 0x3BE30(&state, player) is the top screen of OPTION MODE: a loop of {0x3AC50 controller notice, 0x520A0(1) wait,
//    0x3AB40 background, 0x3BB00(player) pad, drawing, reaction}. [0x3369C7] is the selected row, [0x3369AD] the screen
//    the caller (0x3DBD0) shows next: 1 GAME CONFIG, 3 AUDIO CONFIG, 4 EXTRA CONFIG, 5 RECORDS, 0xFF leave.
//  * 0x3BB00(player) returns the pressed key: 1 up, 2 down, 4 left, 3 right, 5 or 0xD confirm, 6 back.
//  * 0x5E740(text, x, y, color) draws big text (2 cells a character), 0x5E380(text, x, y, color) small text (1 cell).
//  * 0x4FA70(id, n) plays a sound: 0x23 cursor, 0x24 confirm, 0x29 back, (0xFFFF, 2) stops the current one.
//
// The top screen is replaced here by the same loop with 6 rows. The new row runs a settings screen written in the same
// style as GAME CONFIG, which changes the runtime's settings (the ones in the F10 menu) and saves them on leaving.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kControllerNotice = 0x0003AC50;
constexpr uint32_t kWaitFrames = 0x000520A0;
constexpr uint32_t kBackground = 0x0003AB40;
constexpr uint32_t kReadPad = 0x0003BB00;
constexpr uint32_t kTextBig = 0x0005E740;
constexpr uint32_t kTextSmall = 0x0005E380;
constexpr uint32_t kSound = 0x0004FA70;

constexpr uint32_t kSelected = 0x003369C7;  // byte: selected row of the top screen
constexpr uint32_t kNextScreen = 0x003369AD;  // byte: screen the caller shows next
constexpr uint32_t kUnlocked = 0x00336988;  // word: 0xFFC0 while EXTRA CONFIG is locked
constexpr uint32_t kLeaveFlag = 0x0030C680;  // byte: 5 ends the screen

constexpr uint32_t kStrOptionMode = 0x0022D814;
constexpr uint32_t kStrGame = 0x0022D804;
constexpr uint32_t kStrAudio = 0x0022D7F4;
constexpr uint32_t kStrExtra = 0x0022D7E4;
constexpr uint32_t kStrExtraLocked = 0x0022D7D4;
constexpr uint32_t kStrRecords = 0x0022D7CC;
constexpr uint32_t kStrExit = 0x0022CA70;
constexpr uint32_t kStrCursor = 0x0022D7C8;

enum Key : uint8_t { kUp = 1, kDown = 2, kRight = 3, kLeft = 4, kConfirm = 5, kBack = 6, kStart = 0xD };

struct Game {
    X86Context& ctx;
    uint32_t scratch;  // guest memory for the texts of this file

    uint32_t Call(uint32_t fn, std::initializer_list<uint32_t> args = {}) { return CallGuest(ctx, fn, args); }
    void Sound(uint32_t id, uint32_t n) { Call(kSound, {id, n}); }
    void Frame() {
        Call(kControllerNotice);
        Call(kWaitFrames, {1});
        Call(kBackground);
    }
    uint8_t Pad(uint32_t player) { return uint8_t(Call(kReadPad, {player})); }
    uint32_t Put(int slot, const std::string& text) {  // a text in guest memory, 40 bytes a slot
        uint32_t at = scratch + 40u * slot;
        std::memset(HostPtr(at), 0, 40);
        std::memcpy(HostPtr(at), text.c_str(), std::min<size_t>(text.size(), 39));
        return at;
    }
    void Big(uint32_t text, int x, int y, int color) {
        Call(kTextBig, {text, uint32_t(x), uint32_t(y), uint32_t(color)});
    }
    void Small(uint32_t text, int x, int y, int color) {
        Call(kTextSmall, {text, uint32_t(x), uint32_t(y), uint32_t(color)});
    }
};

uint32_t Scratch() {
    static uint32_t at = mem::AllocSystem(40 * 24);
    return at;
}

// ---- the settings screen ----------------------------------------------------------------------------------------

constexpr int kRows = 8;
constexpr const char* kLabels[kRows] = {"DISPLAY MODE", "RESOLUTION",   "VSYNC",           "FRAME RATE",
                                        "RENDER SCALE", "ANISOTROPIC", "AUDIO OUTPUT",    "CUSTOM TEXTURES"};
constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};

using Size = std::pair<uint32_t, uint32_t>;
std::vector<Size> g_sizes;  // what RESOLUTION cycles through; {0, 0} is the desktop's own mode

// The sizes to offer: the modes this monitor lists, and the usual 4:3 sizes. A real mode change (fullscreen) only
// works with sizes the monitor lists; a window or the borderless output takes any size up to the desktop's.
std::vector<Size> BuildSizes(app::DisplayMode mode) {
    std::vector<Size> listed;
    HMONITOR monitor = MonitorFromWindow(HWND(app::Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(info.szDevice, i, &dm); i++)
            if (dm.dmBitsPerPel >= 32 && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480)
                listed.push_back({dm.dmPelsWidth, dm.dmPelsHeight});
    }
    const app::Mode desktop = app::Desktop();
    std::vector<Size> sizes;
    if (mode == app::DisplayMode::Fullscreen) {
        sizes = listed;
    } else {
        for (const Size& s : listed)
            if (s.first <= desktop.width && s.second <= desktop.height) sizes.push_back(s);
        static const Size k43[] = {{640, 480},   {800, 600},   {1024, 768},  {1152, 864}, {1280, 960},
                                   {1400, 1050}, {1600, 1200}, {1920, 1440}, {2048, 1536}};
        for (const Size& s : k43)
            if (s.first <= desktop.width && s.second <= desktop.height) sizes.push_back(s);
    }
    std::sort(sizes.begin(), sizes.end(), [](const Size& a, const Size& b) {
        return uint64_t(a.first) * a.second != uint64_t(b.first) * b.second ? uint64_t(a.first) * a.second < uint64_t(b.first) * b.second
                                                                              : a.first < b.first;
    });
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    sizes.erase(std::remove(sizes.begin(), sizes.end(), Size{desktop.width, desktop.height}), sizes.end());  // "desktop" is it
    sizes.insert(sizes.begin(), Size{0, 0});
    return sizes;
}

std::string Value(int row) {
    std::lock_guard<std::recursive_mutex> lock(app::UiLock());
    const app::Settings& s = app::Get();
    switch (row) {
        case 0: {
            static const char* const modes[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
            return modes[std::min(int(s.display_mode), 2)];
        }
        case 1: {
            if (!s.width || !s.height) return "DESKTOP";
            std::string text = std::to_string(s.width) + "X" + std::to_string(s.height);
            if (uint64_t(s.width) * 3 == uint64_t(s.height) * 4) text += " 4:3";
            return text;
        }
        case 2: return s.vsync ? "ON" : "OFF";
        case 3: return s.fps_cap == 60 ? "60 FPS" : s.fps_cap == app::kFpsCapRefresh ? "MONITOR" : s.fps_cap == 0 ? "UNLIMITED" : "CUSTOM";
        case 4: {
            char text[16];
            std::snprintf(text, sizeof(text), "%u%%", unsigned(s.render_percent));
            return text;
        }
        case 5: return s.aniso == 0 ? "OFF" : std::to_string(s.aniso) + "X";
        case 6: {
            static const char* const outputs[] = {"AUTO", "STEREO", "SURROUND", "MONO"};
            return outputs[std::min<uint32_t>(s.audio_output, 3)];
        }
        case 7: return s.custom_textures ? "ON" : "OFF";
    }
    return "";
}

void Change(int row, int dir) {
    std::lock_guard<std::recursive_mutex> lock(app::UiLock());
    app::Settings& s = app::Get();
    switch (row) {
        case 0:
            s.display_mode = app::DisplayMode((int(s.display_mode) + dir + 3) % 3);
            g_sizes = BuildSizes(s.display_mode);
            // a size the new mode does not offer goes back to the desktop's
            if (std::find(g_sizes.begin(), g_sizes.end(), Size{s.width, s.height}) == g_sizes.end()) s.width = s.height = 0;
            app::ApplyDisplay();
            break;
        case 1: {
            if (g_sizes.empty()) g_sizes = BuildSizes(s.display_mode);
            auto at = std::find(g_sizes.begin(), g_sizes.end(), Size{s.width, s.height});
            int i = at == g_sizes.end() ? 0 : int(at - g_sizes.begin());
            const int n = int(g_sizes.size());
            i = (i + dir + n) % n;
            s.width = g_sizes[i].first;
            s.height = g_sizes[i].second;
            app::ApplyDisplay();
            break;
        }
        case 2: s.vsync = !s.vsync; break;
        case 3: {
            int i = s.fps_cap == app::kFpsCapRefresh ? 1 : s.fps_cap == 0 ? 2 : 0;
            i = (i + dir + 3) % 3;
            s.fps_cap = i == 0 ? 60 : i == 1 ? app::kFpsCapRefresh : 0;
            break;
        }
        case 4: s.render_percent = uint32_t(std::clamp(int(s.render_percent) + 25 * dir, 25, 400)); break;
        case 5: {
            int i = 0;
            for (int k = 0; k < 5; k++)
                if (s.aniso == kAniso[k]) i = k;
            s.aniso = kAniso[(i + dir + 5) % 5];
            break;
        }
        case 6:
            s.audio_output = uint32_t((int(s.audio_output) + dir + 4) % 4);
            audio::SetOutputMode(s.audio_output);
            break;
        case 7: s.custom_textures = !s.custom_textures; break;
    }
}

void SettingsScreen(Game& g, uint32_t player) {
    {
        std::lock_guard<std::recursive_mutex> lock(app::UiLock());
        g_sizes = BuildSizes(app::Get().display_mode);  // the monitor may have changed since the last time
    }
    int sel = 0;
    const int count = kRows + 1;  // the last row is EXIT
    for (;;) {
        g.Frame();
        const uint8_t key = g.Pad(player);
        bool leave = false;
        if (key == kUp) {
            g.Sound(0x23, 1);
            sel = (sel + count - 1) % count;
        } else if (key == kDown) {
            g.Sound(0x23, 1);
            sel = (sel + 1) % count;
        } else if (key == kBack) {
            leave = true;
        } else if (key == kLeft || key == kRight || key == kConfirm || key == kStart) {
            if (sel == kRows) {
                leave = key == kConfirm || key == kStart;
            } else {
                g.Sound(0x23, 1);
                Change(sel, key == kLeft ? -1 : 1);
            }
        }

        g.Small(g.Put(0, "RECOMP SETTINGS"), 15, 2, 5);
        for (int i = 0; i < kRows; i++) {
            g.Small(g.Put(1 + i, kLabels[i]), 7, 9 + 2 * i, 3);
            g.Small(g.Put(9 + i, Value(i)), 29, 9 + 2 * i, 4);
        }
        g.Small(kStrExit, 7, 9 + 2 * kRows + 1, 3);
        g.Small(kStrCursor, 5, sel == kRows ? 9 + 2 * kRows + 1 : 9 + 2 * sel, 4);

        if (leave) {
            {
                std::lock_guard<std::recursive_mutex> lock(app::UiLock());
                app::Save();
            }
            g.Sound(0xFFFF, 2);
            g.Sound(0x29, 1);
            return;
        }
    }
}

// ---- the top screen of OPTION MODE ------------------------------------------------------------------------------

constexpr int kTopRows = 6;  // GAME, AUDIO, EXTRA, RECORDS, RECOMP SETTINGS, EXIT
constexpr int kTopY[kTopRows] = {11, 13, 15, 17, 19, 22};

void DrawTop(Game& g, int sel) {
    const bool locked = R16(kUnlocked) == 0xFFC0;
    g.Big(kStrOptionMode, 11, 7, 5);
    g.Big(kStrGame, 10, kTopY[0], 3);
    g.Big(kStrAudio, 10, kTopY[1], 3);
    g.Big(locked ? kStrExtraLocked : kStrExtra, 10, kTopY[2], 3);
    g.Big(kStrRecords, 10, kTopY[3], 3);
    g.Big(g.Put(20, "RECOMP SETTINGS"), 10, kTopY[4], 3);
    g.Big(kStrExit, 10, kTopY[5], 3);
    g.Small(kStrCursor, 8, kTopY[std::clamp(sel, 0, kTopRows - 1)], 4);
}

}  // namespace

// void OptionsTop(void* state, int player), cdecl: esp -> [return][state][player]
X86_FUNC(sub_0003BE30) {
    Game g{ctx, Scratch()};
    const uint32_t player = R32(ctx.esp + 8);
    bool finish = false;
    uint8_t& selected = G<uint8_t>(kSelected);
    if (selected >= kTopRows) selected = 0;

    for (;;) {
        g.Frame();
        if (G<uint8_t>(kLeaveFlag) == 5) finish = true;
        const uint8_t key = g.Pad(player);
        const bool locked = R16(kUnlocked) == 0xFFC0;

        if (key == kUp) {
            g.Sound(0x23, 1);
            int sel = int(selected) - 1;
            if (locked && sel == 2) sel = 1;
            else if (sel < 0) sel = kTopRows - 1;
            selected = uint8_t(sel);
        } else if (key == kDown) {
            g.Sound(0x23, 1);
            int sel = int(selected) + 1;
            if (locked && sel == 2) sel = 3;
            else if (sel >= kTopRows) sel = 0;
            selected = uint8_t(sel);
        } else if (key == kBack) {
            g.Sound(0xFFFF, 2);
            g.Sound(0x29, 1);
            selected = 0;
            G<uint8_t>(kNextScreen) = 0xFF;
            DrawTop(g, 0);
            ctx.esp += 4;
            return;
        } else if (key == kConfirm || key == kStart) {
            g.Sound(0xFFFF, 2);
            g.Sound(0x24, 1);
            const int sel = selected;
            if (sel == 4) {
                SettingsScreen(g, player);  // ours, then back to this screen
            } else {
                // GAME CONFIG 1, AUDIO CONFIG 3, EXTRA CONFIG 4, RECORDS 5, EXIT leaves
                static const uint8_t screens[] = {1, 3, 4, 5, 0, 0xFF};
                if (sel == 5) {
                    selected = 0;
                    G<uint8_t>(kNextScreen) = 0xFF;
                } else {
                    G<uint8_t>(kNextScreen) = screens[sel];
                }
                DrawTop(g, sel);
                ctx.esp += 4;
                return;
            }
        }

        DrawTop(g, selected);
        if (finish) {
            ctx.esp += 4;
            return;
        }
    }
}
