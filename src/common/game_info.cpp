#include "app/app.h"
#include "menu.h"

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Dead or Alive 1 Ultimate",  // window and splash
    .short_title = L"DOA1U",               // message boxes
    .name = L"DOA1U",                      // window classes, crash dumps, handover event
    .log = "doa1u",                        // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or Doa1.xbe with its folders beside it.",
    .images = {{"Doa1.xbe", "DOA1U"}},  // each recompiled xbe and its exe, for title launches
    .options = {},                        // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = menu::Message,             // window messages first; F10 opens the menu
    .ui = menu::Ui,                       // ImGui drawing each frame
    .pad = menu::Pad,                      // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
    .widescreen = false,                  // 4:3 only game: shown 4:3 with bars, never stretched
    .network = false,                     // offline: no sockets, no Windows firewall prompt
};
