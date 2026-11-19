// Keyboard as a player's controller through GameInfo::pad, with the keys each action takes kept in settings.toml.
// The keys are read with GetAsyncKeyState, only while this process's window has the focus.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "keyboard.h"

namespace keyboard {

namespace {

// buttons bit: 0 dpad up, 1 down, 2 left, 3 right, 4 start, 5 back, 6 left thumb, 7 right thumb
// analog index: 0 A, 1 B, 2 X, 3 Y, 4 black, 5 white, 6 left trigger, 7 right trigger
//
// The fight actions go to whichever button the game's BUTTON CONFIG gives them, read from the game each time
// (Doa1.xbe): byte [0x336914 + player * 7 + action] is the button of PUNCH, KICK, HOLD, P+K, H+P, H+K, H+P+K, 0xFF
// none, in the game's order of buttons A, B, BLACK, X, Y, WHITE, L, R (the Saturn's A B C X Y Z L R); dword
// [0x30F9B4 + player * 4] is the controller port a player plays on, 6 none. An action with no button of its own
// presses those of its parts (P+K: PUNCH's and KICK's), as a player would.
struct Action {
    const char* name;  // on screen
    const char* key;   // in settings.toml
    int bits;          // buttons bits pressed, 0 none
    int analog;        // analog index, or -1
    int fight;         // BUTTON CONFIG action, or -1
    int defaults[kSlots];
};

constexpr Action kButtons[] = {
    {"UP", "up", 1 << 0, -1, -1, {'W', VK_UP}},
    {"DOWN", "down", 1 << 1, -1, -1, {'S', VK_DOWN}},
    {"LEFT", "left", 1 << 2, -1, -1, {'A', VK_LEFT}},
    {"RIGHT", "right", 1 << 3, -1, -1, {'D', VK_RIGHT}},
    {"PUNCH", "punch", 0, -1, 0, {'K', 0}},
    {"KICK", "kick", 0, -1, 1, {'L', 0}},
    {"HOLD", "hold", 0, -1, 2, {'J', 0}},
    {"P+K", "punch_kick", 0, -1, 3, {'I', 0}},
    {"H+P", "hold_punch", 0, -1, 4, {'U', 0}},
    {"H+K", "hold_kick", 0, -1, 5, {'O', 0}},
    {"H+P+K", "hold_punch_kick", 0, -1, 6, {'P', 0}},
    {"CONFIRM", "confirm", 0, 0, -1, {VK_RETURN, 0}},  // the A button, as the menus take it
    {"CANCEL", "cancel", 0, 1, -1, {VK_BACK, 0}},      // the B button
    {"START", "start", 1 << 4, -1, -1, {VK_SPACE, 0}},
    {"BACK", "back", 1 << 5, -1, -1, {0, 0}},
    // the game's own way out of a fight: BACK and START held together; in a fight only (START confirms in menus)
    {"MAIN MENU", "main_menu", (1 << 4) | (1 << 5), -1, -1, {VK_ESCAPE, 0}},
};
constexpr uint32_t kButtonConfig = 0x00336914;
constexpr uint32_t kPlayerPort = 0x0030F9B4;
// What a fight reads (0x3B170): byte [0x307620 + port * 8 + button] holds the actions a button makes, bits H 1,
// P 2, K 4; the game builds it from BUTTON CONFIG (0x3AF10). An action is its bits: PUNCH 2, KICK 4, HOLD 1, P+K 6,
// H+P 3, H+K 5, H+P+K 7.
constexpr uint32_t kButtonBits = 0x00307620;
constexpr uint8_t kActionBits[7] = {2, 4, 1, 6, 3, 5, 7};
constexpr int kGameButtonAnalog[8] = {0, 1, 4, 2, 3, 5, 6, 7};  // the game's button order to the pad's analog index

std::atomic<int> g_port{0};  // the console port the keyboard plays on (players.cpp), -1 none

// the player on the keyboard's port: the first when the game has none there
int KeyboardPlayer() {
    if (!kernel::g_base) return 0;
    for (int p = 0; p < 2; p++)
        if (int(kernel::R32(kPlayerPort + 4 * p)) == g_port.load()) return p;
    return 0;
}

// Presses the buttons that make an action on the keyboard's port: the one button that makes exactly it, else one for each of its
// parts (H, P, K). The table is read as the fight reads it; before the game has built it (all 0), BUTTON CONFIG.
void PressAction(int action, input::Pad& pad) {
    if (!kernel::g_base) return;
    const uint8_t want = kActionBits[action];
    uint8_t bits[8];
    bool built = false;
    for (int b = 0; b < 8; b++) {
        bits[b] = kernel::G<uint8_t>(kButtonBits + uint32_t(g_port.load()) * 8 + uint32_t(b)) & 7;
        built |= bits[b] != 0;
    }
    if (!built) {
        const int player = KeyboardPlayer();
        for (int a = 0; a < 7; a++) {
            const uint8_t b = kernel::G<uint8_t>(kButtonConfig + uint32_t(player * 7 + a));
            if (b < 8) bits[b] |= kActionBits[a];
        }
    }
    static int logged = 0;  // what the keyboard found, the first times
    if (logged < 8) {
        logged++;
        LOGI("keyboard: action %d wants %u; buttons %u %u %u %u %u %u %u %u%s; ports %u %u", action, want, bits[0],
             bits[1], bits[2], bits[3], bits[4], bits[5], bits[6], bits[7], built ? "" : " (from BUTTON CONFIG)",
             kernel::R32(kPlayerPort), kernel::R32(kPlayerPort + 4));
    }
    for (int b = 0; b < 8; b++)
        if (bits[b] == want) {
            pad.analog[kGameButtonAnalog[b]] = 0xFF;
            return;
        }
    for (uint8_t part : {uint8_t(1), uint8_t(2), uint8_t(4)}) {
        if (!(want & part)) continue;
        for (int b = 0; b < 8; b++)
            if (bits[b] == part) {
                pad.analog[kGameButtonAnalog[b]] = 0xFF;
                break;
            }
    }
}

constexpr int kCount = int(sizeof(kButtons) / sizeof(kButtons[0]));
constexpr int kMainMenu = kCount - 1;

// A fight is on while its input task lives. The game runs its frame work as tasks, each a fiber in a slot of 0x20
// bytes at 0x6E95C0 ([+0] flags, bit 0 the task exists; [+0xC] its routine; 0x52240 starts one, 0x52080 ends it).
// The fight starts 0x16300, which reads the players' pads, in slot 5 (0x33696) with its other tasks.
constexpr uint32_t kTaskSlots = 0x006E95C0, kFightSlot = 5, kFightTask = 0x00016300;
bool InFight() {
    if (!kernel::g_base) return false;
    const uint32_t slot = kTaskSlots + kFightSlot * 0x20;
    return (kernel::R32(slot) & 1) && kernel::R32(slot + 0xC) == kFightTask;
}

std::mutex g_lock;
int g_keys[kCount][kSlots];
bool g_loaded = false;
std::atomic<bool> g_capturing{false};
std::atomic<bool> g_quiet{false};  // after a capture, until every key is up
bool g_down[256] = {};             // PressedKey's last look

void Defaults() {
    for (int b = 0; b < kCount; b++)
        for (int s = 0; s < kSlots; s++) g_keys[b][s] = kButtons[b].defaults[s];
}

void EnsureLoaded() {
    if (!g_loaded) {
        Defaults();
        g_loaded = true;
    }
}

bool Valid(int vk) {
    // Esc can't be chosen on the key screen (it cancels there), but MAIN MENU has it by default
    return vk > VK_XBUTTON2 && vk < 256 && vk != VK_F10 && vk != VK_SHIFT && vk != VK_CONTROL &&
           vk != VK_MENU;
}

bool WindowHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool AnyKeyDown() {
    for (int vk = VK_XBUTTON2 + 1; vk < 256; vk++)
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

}  // namespace

int Count() { return kCount; }
const char* Name(int button) { return button >= 0 && button < kCount ? kButtons[button].name : ""; }

int Key(int button, int slot) {
    if (button < 0 || button >= kCount || slot < 0 || slot >= kSlots) return 0;
    std::lock_guard<std::mutex> lock(g_lock);
    EnsureLoaded();
    return g_keys[button][slot];
}

void Set(int button, int slot, int vk) {
    if (button < 0 || button >= kCount || slot < 0 || slot >= kSlots) return;
    if (vk && !Valid(vk)) return;
    std::lock_guard<std::mutex> lock(g_lock);
    EnsureLoaded();
    if (vk)  // one key, one button
        for (int b = 0; b < kCount; b++)
            for (int s = 0; s < kSlots; s++)
                if (g_keys[b][s] == vk) g_keys[b][s] = 0;
    g_keys[button][slot] = vk;
}

void ResetDefaults() {
    std::lock_guard<std::mutex> lock(g_lock);
    Defaults();
    g_loaded = true;
}

std::string KeyName(int vk) {
    if (!vk) return "-";
    switch (vk) {  // short names: the small font has room for 9 characters a column
        case VK_UP: return "UP";
        case VK_DOWN: return "DOWN";
        case VK_LEFT: return "LEFT";
        case VK_RIGHT: return "RIGHT";
        case VK_RETURN: return "ENTER";
        case VK_BACK: return "BACKSPACE";
        case VK_SPACE: return "SPACE";
        case VK_TAB: return "TAB";
        case VK_ESCAPE: return "ESC";
        case VK_LSHIFT: return "L SHIFT";
        case VK_RSHIFT: return "R SHIFT";
        case VK_LCONTROL: return "L CTRL";
        case VK_RCONTROL: return "R CTRL";
        case VK_LMENU: return "L ALT";
        case VK_RMENU: return "R ALT";
        case VK_INSERT: return "INSERT";
        case VK_DELETE: return "DELETE";
        case VK_HOME: return "HOME";
        case VK_END: return "END";
        case VK_PRIOR: return "PAGE UP";
        case VK_NEXT: return "PAGE DOWN";
        case VK_DIVIDE: return "NUM /";
        case VK_MULTIPLY: return "NUM *";
        case VK_SUBTRACT: return "NUM -";
        case VK_ADD: return "NUM +";
        case VK_DECIMAL: return "NUM .";
        case VK_CAPITAL: return "CAPS LOCK";
        case VK_LWIN: return "L WIN";
        case VK_RWIN: return "R WIN";
        case VK_APPS: return "MENU";
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUM " + std::to_string(vk - VK_NUMPAD0);
    std::string name;
    if (UINT scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) {
        wchar_t text[64];
        if (int n = GetKeyNameTextW(LONG(scan << 16), text, 64); n > 0)
            for (int i = 0; i < n; i++) name += text[i] < 128 ? char(text[i]) : '?';  // the game's font is ASCII
    }
    if (name.empty()) name = "KEY " + std::to_string(vk);
    for (char& c : name) c = char(std::toupper(uint8_t(c)));
    if (name.size() > 9) name.resize(9);
    return name;
}

void SetCapturing(bool capturing) {
    if (capturing) {
        // what is down now (the key that asked) doesn't count
        for (int vk = 0; vk < 256; vk++) g_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    } else {
        g_quiet = true;
    }
    g_capturing = capturing;
}

int PressedKey() {
    if (!WindowHasFocus()) return 0;
    int pressed = 0;
    for (int vk = VK_XBUTTON2 + 1; vk < 256; vk++) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        // the generic Shift, Ctrl and Alt also go down with their left or right ones, which are what is kept
        if (down && !g_down[vk] && !pressed && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) pressed = vk;
        g_down[vk] = down;
    }
    return pressed;
}

void SetPort(int port) { g_port = port >= 0 && port < 4 ? port : -1; }
int Port() { return g_port; }

void Apply(int port, input::Pad& pad) {
    if (port != g_port.load()) return;
    pad.connected = true;  // the keyboard is always there
    if (!WindowHasFocus() || g_capturing) return;
    if (g_quiet) {
        if (AnyKeyDown()) return;
        g_quiet = false;
    }
    std::lock_guard<std::mutex> lock(g_lock);
    EnsureLoaded();
    for (int b = 0; b < kCount; b++) {
        bool down = false;
        for (int s = 0; s < kSlots; s++)
            if (g_keys[b][s] && (GetAsyncKeyState(g_keys[b][s]) & 0x8000)) down = true;
        if (!down) continue;
        if (b == kMainMenu && !InFight()) continue;
        pad.buttons |= uint16_t(kButtons[b].bits);
        if (kButtons[b].analog >= 0) pad.analog[kButtons[b].analog] = 0xFF;
        if (kButtons[b].fight >= 0) PressAction(kButtons[b].fight, pad);
    }
}

void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    std::lock_guard<std::mutex> lock(g_lock);
    Defaults();
    g_loaded = true;
    const toml::table* kb = t["keyboard"].as_table();
    if (!kb) return;
    for (int b = 0; b < kCount; b++) {
        const toml::array* keys = (*kb)[kButtons[b].key].as_array();
        if (!keys) continue;  // a button the file doesn't name keeps its defaults
        for (int s = 0; s < kSlots; s++) {
            const int vk = s < int(keys->size()) ? int((*keys)[size_t(s)].value_or(int64_t(0))) : 0;
            g_keys[b][s] = Valid(vk) ? vk : 0;
        }
    }
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    std::lock_guard<std::mutex> lock(g_lock);
    EnsureLoaded();
    toml::table kb;
    for (int b = 0; b < kCount; b++) {
        toml::array keys;
        for (int s = 0; s < kSlots; s++)
            if (int vk = g_keys[b][s]) keys.push_back(int64_t(vk));
        kb.insert(kButtons[b].key, std::move(keys));
    }
    t.insert_or_assign("keyboard", std::move(kb));
}

}  // namespace keyboard
