// "RECOMP SETTINGS" and "INPUT" (players' devices, KEYBOARD KEYS) in the OPTION MODE screen, between RECORDS and EXIT, drawn with the game's own
// text routines.

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
#include "../common/keyboard.h"
#include "../common/players.h"

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
    uint32_t Put(int slot, const std::string& text) {  // a text in guest memory, 40 bytes a slot, kTextSlots of them
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

constexpr int kTextSlots = 64;
uint32_t Scratch() {
    static uint32_t at = mem::AllocSystem(40 * kTextSlots);
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

// ---- the keyboard keys screen ------------------------------------------------------------------------------------
//
// A row an action (movement, the fight actions of BUTTON CONFIG, the menu buttons) with its two keys, then RESET TO
// DEFAULTS and EXIT. Up and down choose the row, left and right
// the key; A (or START) waits for a key: Esc cancels, Delete clears. B leaves and saves.

constexpr int kKeysVisible = 9;  // button rows on screen at a time
constexpr int kKeysY = 6, kKeysExitY = 25, kKeysHelpY = 27;
constexpr int kKeyNameX = 5, kKey1X = 20, kKey2X = 30;

void DrawKeys(Game& g, int sel, int slot, int top, int capture_slot, const char* help) {
    const int buttons = keyboard::Count();
    const int rows = buttons + 1;  // and RESET TO DEFAULTS; EXIT apart
    int t = 24;                    // text slots 24 on are this screen's
    g.Small(g.Put(t++, "KEYBOARD KEYS"), 14, 2, 5);
    int cursor_y = kKeysExitY;
    for (int k = 0; k < kKeysVisible && top + k < rows; k++) {
        const int row = top + k;
        const int y = kKeysY + 2 * k;
        if (row == sel) cursor_y = y;
        if (row == buttons) {
            g.Small(g.Put(t++, "RESET TO DEFAULTS"), kKeyNameX, y, 3);
            continue;
        }
        g.Small(g.Put(t++, keyboard::Name(row)), kKeyNameX, y, 3);
        for (int s = 0; s < keyboard::kSlots; s++) {
            const bool cell = row == sel && s == slot;
            const std::string key =
                cell && capture_slot >= 0 ? "PRESS KEY" : keyboard::KeyName(keyboard::Key(row, s));
            g.Small(g.Put(t++, key), s ? kKey2X : kKey1X, y, cell ? 4 : 3);
        }
    }
    g.Small(kStrExit, kKeyNameX, kKeysExitY, 3);
    g.Small(kStrCursor, kKeyNameX - 2, cursor_y, 4);
    g.Small(g.Put(t++, help), 2, kKeysHelpY, 3);
}

void KeysScreen(Game& g, uint32_t player) {
    const int buttons = keyboard::Count();
    const int count = buttons + 2;  // RESET TO DEFAULTS, EXIT
    int sel = 0, slot = 0, top = 0;
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
        } else if ((key == kLeft || key == kRight) && sel < buttons) {
            g.Sound(0x23, 1);
            slot ^= 1;
        } else if (key == kBack) {
            leave = true;
        } else if (key == kConfirm || key == kStart) {
            if (sel == count - 1) {
                leave = true;
            } else if (sel == buttons) {
                g.Sound(0x24, 1);
                keyboard::ResetDefaults();
            } else {
                // wait for a key, drawing as we go; the pad's B cancels too
                g.Sound(0x24, 1);
                keyboard::SetCapturing(true);
                for (;;) {
                    g.Frame();
                    if (sel < top) top = sel;
                    if (sel >= top + kKeysVisible && sel < buttons + 1) top = sel - kKeysVisible + 1;
                    DrawKeys(g, sel, slot, top, slot, "ESC: CANCEL   DELETE: CLEAR");
                    const int vk = keyboard::PressedKey();
                    const uint8_t pad = g.Pad(player);
                    if (vk == VK_ESCAPE || pad == kBack) {
                        g.Sound(0x29, 1);
                        break;
                    }
                    if (vk == VK_DELETE) {
                        keyboard::Set(sel, slot, 0);
                        g.Sound(0x23, 1);
                        break;
                    }
                    if (vk && vk != VK_F10) {
                        keyboard::Set(sel, slot, vk);
                        g.Sound(0x24, 1);
                        break;
                    }
                }
                keyboard::SetCapturing(false);
            }
        }

        if (sel < buttons + 1) {
            if (sel < top) top = sel;
            if (sel >= top + kKeysVisible) top = sel - kKeysVisible + 1;
        }
        DrawKeys(g, sel, slot, top, -1, "A: SET KEY   LEFT/RIGHT: KEY 1 OR 2");

        if (leave) {
            {
                std::lock_guard<std::recursive_mutex> lock(app::UiLock());
                app::Save();  // [keyboard], through GameInfo::save
            }
            g.Sound(0xFFFF, 2);
            g.Sound(0x29, 1);
            return;
        }
    }
}

// ---- the input screen ---------------------------------------------------------------------------------------------
//
// PLAYER 1 to 4, each player's devices (left and right, or A, step through them: KEYBOARD + CONTROLLER 1-4,
// CONTROLLER 1-4, KEYBOARD, NONE; a device another player had goes to them in exchange), KEYBOARD KEYS and EXIT.
// B leaves and saves.

constexpr int kInputY = 9, kInputValueX = 15;

void DrawInput(Game& g, int sel) {
    int t = 24;
    g.Small(g.Put(t++, "INPUT"), 17, 2, 5);
    for (int p = 0; p < players::kPlayers; p++) {
        const int y = kInputY + 2 * p;
        const int choice = players::Choice(p);
        g.Small(g.Put(t++, "PLAYER " + std::to_string(p + 1)), 5, y, 3);
        g.Small(g.Put(t++, players::ChoiceName(choice)), kInputValueX, y, sel == p ? 4 : 3);
    }
    const int keys_y = kInputY + 2 * players::kPlayers + 1;
    const int exit_y = keys_y + 3;
    g.Small(g.Put(t++, "KEYBOARD KEYS"), 5, keys_y, 3);
    g.Small(kStrExit, 5, exit_y, 3);
    const int cursor_y = sel < players::kPlayers ? kInputY + 2 * sel : sel == players::kPlayers ? keys_y : exit_y;
    g.Small(kStrCursor, 3, cursor_y, 4);
    std::string help;
    if (sel < players::kPlayers) {
        const int choice = players::Choice(sel);
        help = players::Missing(choice)
                   ? "CONTROLLER " + std::to_string(players::ControllerOf(choice) + 1) + " IS NOT PLUGGED IN"
                   : "LEFT/RIGHT: CHANGE DEVICES";
    }
    g.Small(g.Put(t++, help), 2, kKeysHelpY, 3);
}

void InputScreen(Game& g, uint32_t player) {
    const int count = players::kPlayers + 2;  // KEYBOARD KEYS, EXIT
    int sel = 0;
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
        } else if (sel < players::kPlayers && (key == kLeft || key == kRight || key == kConfirm)) {
            g.Sound(0x23, 1);
            players::Step(sel, key == kLeft ? -1 : 1);
        } else if (key == kConfirm || key == kStart) {
            if (sel == players::kPlayers) {
                g.Sound(0x24, 1);
                KeysScreen(g, player);
            } else if (sel == count - 1) {
                leave = true;
            }
        }
        DrawInput(g, sel);
        if (leave) {
            {
                std::lock_guard<std::recursive_mutex> lock(app::UiLock());
                app::Save();  // [players] and [keyboard], through GameInfo::save
            }
            g.Sound(0xFFFF, 2);
            g.Sound(0x29, 1);
            return;
        }
    }
}

// ---- the top screen of OPTION MODE ------------------------------------------------------------------------------

constexpr int kTopRows = 7;  // GAME, AUDIO, EXTRA, RECORDS, RECOMP SETTINGS, INPUT, EXIT
constexpr int kTopY[kTopRows] = {11, 13, 15, 17, 19, 21, 24};

void DrawTop(Game& g, int sel) {
    const bool locked = R16(kUnlocked) == 0xFFC0;
    g.Big(kStrOptionMode, 11, 7, 5);
    g.Big(kStrGame, 10, kTopY[0], 3);
    g.Big(kStrAudio, 10, kTopY[1], 3);
    g.Big(locked ? kStrExtraLocked : kStrExtra, 10, kTopY[2], 3);
    g.Big(kStrRecords, 10, kTopY[3], 3);
    g.Big(g.Put(20, "RECOMP SETTINGS"), 10, kTopY[4], 3);
    g.Big(g.Put(21, "INPUT"), 10, kTopY[5], 3);
    g.Big(kStrExit, 10, kTopY[6], 3);
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
            } else if (sel == 5) {
                InputScreen(g, player);
            } else {
                // GAME CONFIG 1, AUDIO CONFIG 3, EXTRA CONFIG 4, RECORDS 5, EXIT leaves
                static const uint8_t screens[] = {1, 3, 4, 5, 0, 0, 0xFF};
                if (sel == 6) {
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
