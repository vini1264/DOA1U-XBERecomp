// Keyboard as the controller on port 0, through GameInfo::pad. The keys are read with GetAsyncKeyState, only
// while this process's window has the focus.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

#include "core/input.h"
#include "keyboard.h"

namespace keyboard {

namespace {

// buttons bit: 0 dpad up, 1 down, 2 left, 3 right, 4 start, 5 back, 6 left thumb, 7 right thumb
// analog index: 0 A, 1 B, 2 X, 3 Y, 4 black, 5 white, 6 left trigger, 7 right trigger
struct Binding {
    int vk;
    int button;  // buttons bit, or -1
    int analog;  // analog index, or -1
};

constexpr Binding kBindings[] = {
    // directions: arrows or WASD
    {VK_UP, 0, -1},   {'W', 0, -1},
    {VK_DOWN, 1, -1}, {'S', 1, -1},
    {VK_LEFT, 2, -1}, {'A', 2, -1},
    {VK_RIGHT, 3, -1}, {'D', 3, -1},
    // start and back
    {VK_RETURN, 4, -1}, {VK_BACK, 5, -1},
    // face buttons: Z X C V under the left hand, K L J I under the right
    {'Z', -1, 0}, {'K', -1, 0},  // A
    {'X', -1, 1}, {'L', -1, 1},  // B
    {'C', -1, 2}, {'J', -1, 2},  // X
    {'V', -1, 3}, {'I', -1, 3},  // Y
    // black, white and the triggers
    {'U', -1, 4}, {'O', -1, 5}, {'Q', -1, 6}, {'E', -1, 7},
};

bool WindowHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace

void Apply(int port, input::Pad& pad) {
    if (port != 0) return;
    pad.connected = true;  // the keyboard is always there
    if (!WindowHasFocus()) return;
    for (const Binding& b : kBindings) {
        if (!(GetAsyncKeyState(b.vk) & 0x8000)) continue;
        if (b.button >= 0) pad.buttons |= uint16_t(1u << b.button);
        if (b.analog >= 0) pad.analog[b.analog] = 0xFF;
    }
}

}  // namespace keyboard
