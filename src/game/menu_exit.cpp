// An EXIT entry in the main menu (the carousel with ARCADE, DOA ONLINE, TIME ATTACK, V.S., SURVIVAL, KUMITE,
// TRAINING, OPTIONS), after OPTIONS and before ARCADE.
//
// How the menu works (guest addresses):
//  * [0x335994] is the selected entry, 0 ARCADE ... 7 OPTIONS. The loop ends in 0x374F0(selected, 7), which reads the
//    pad, scrolls the label and returns the new entry; it is called from that one place.
//  * 0x374F0 follows a table at 0x2CCEB0, 16 bytes per entry: previous entry, the strip row where its scroll starts,
//    next entry, the same for next. The labels are one strip, the atlas texture 256x256_364b3eba2aef2eaa_DXT3: row 0
//    is a copy of the last entry (so ARCADE can scroll up to it), ARCADE is row 16, DOA ONLINE 32 ... OPTIONS 128.
//  * 0x36F00(row) draws the 16 pixel high window of the strip that starts at that row. The switch in the loop draws the
//    selected label with it, for entries 0 to 7 only, and acts on a confirm: [0x335AE0] is nonzero then.
//
// Here: the table is changed so ARCADE goes back to entry 8 and OPTIONS goes on to it; entry 8 has no table slot
// (the memory after the table belongs to other data), so while it is selected 0x374F0 is run with OPTIONS' slot
// rewritten to describe EXIT; the loop's switch ignores entry 8, so the label is drawn here and a confirm quits.
// The atlas has EXIT at rows 0 and 144 (textures\replacement\256x256_364b3eba2aef2eaa_DXT3.png).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "app/app.h"
#include "xdk/hle.h"

using namespace kernel;

X86_EXTERN_FUNC(__imp__sub_000374F0);

namespace {

constexpr uint32_t kTable = 0x002CCEB0;
constexpr uint32_t kConfirm = 0x00335AE0;  // 16 bit, nonzero while a player confirms
constexpr uint32_t kDrawLabel = 0x00036F00;
constexpr uint32_t kOptions = 7;
constexpr uint32_t kExit = 8;
constexpr uint32_t kMarker = 100;  // added to an entry that 0x374F0 returns for EXIT's scroll, to tell it from "no move"

uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

void SetEntry(uint32_t index, uint32_t prev, float prev_row, uint32_t next, float next_row) {
    const uint32_t at = kTable + 16 * index;
    W32(at + 0, prev);
    W32(at + 4, Bits(prev_row));
    W32(at + 8, next);
    W32(at + 12, Bits(next_row));
}

}  // namespace

// int SelectEntry(int selected, int last), cdecl: esp -> [return][selected][last]
X86_FUNC(sub_000374F0) {
    const uint32_t arg = ctx.esp + 4;
    const uint32_t selected = R32(arg);

    // ARCADE: previous is EXIT (the scroll runs from row 16 up to row 0, which shows EXIT)
    SetEntry(0, kExit, 16.0f, 1, 16.0f);

    if (selected != kExit) {
        // OPTIONS: next is EXIT (the scroll runs from row 128 down to row 144)
        SetEntry(kOptions, 6, 128.0f, kExit, 128.0f);
        __imp__sub_000374F0(ctx, base);
        return;
    }

    // EXIT is selected: ask for OPTIONS' slot, rewritten to describe EXIT. Its previous is OPTIONS (the scroll runs
    // from row 144 up to 128) and its next is ARCADE (from row 0 down to 16). The entries it returns are marked, so
    // a pad that did nothing (the entry we passed comes back) is told apart from a move.
    SetEntry(kOptions, kMarker + kOptions, 144.0f, kMarker + 0, 0.0f);
    W32(arg, kOptions);
    __imp__sub_000374F0(ctx, base);
    SetEntry(kOptions, 6, 128.0f, kExit, 128.0f);
    W32(arg, kExit);

    uint32_t result = ctx.eax;
    if (result >= kMarker) result -= kMarker;  // moved: OPTIONS or ARCADE
    else result = kExit;                       // no move
    ctx.eax = result;
    if (result != kExit) return;

    // still on EXIT: the loop's switch did not draw its label, and a confirm quits
    CallGuest(ctx, kDrawLabel, {Bits(144.0f)});
    ctx.eax = result;
    if (R16(kConfirm) != 0) PostMessageW(HWND(app::Window()), WM_CLOSE, 0, 0);
}
