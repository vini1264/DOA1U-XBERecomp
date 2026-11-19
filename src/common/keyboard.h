#pragma once
#include <string>

namespace input {
struct Pad;
}

// The keyboard as a player's controller (port 0 at first, players.h), through GameInfo::pad: movement, the fight actions (PUNCH, KICK, HOLD and
// their combinations, to the buttons the game's BUTTON CONFIG gives them) and the menu buttons, two keys each, added
// to whatever a real controller reports. The keys live in settings.toml's [keyboard] table and are changed in
// OPTION MODE > KEYBOARD KEYS (options_menu.cpp).
namespace keyboard {

constexpr int kSlots = 2;  // keys per action

int Count();                   // the actions
const char* Name(int button);  // as the game's menus write it, in capitals
int Key(int button, int slot);  // a Windows virtual-key code, 0 for none
void Set(int button, int slot, int vk);  // vk 0 clears; a key taken elsewhere leaves that place
std::string KeyName(int vk);   // "W", "ENTER", "L SHIFT"; "-" for none; 9 characters at most
void ResetDefaults();

// the key screen asks for a binding: the keys reach no button meanwhile, nor until every key is up again
void SetCapturing(bool capturing);
int PressedKey();  // a key that went down since the last call (the guest's thread), 0 for none

// the console port (player) the keyboard plays on, -1 none; port 0 at first (players.cpp sets it)
void SetPort(int port);
int Port();

// GameInfo hooks
void Apply(int port, input::Pad& pad);
void Load(const void* table);
void Save(void* table);
}  // namespace keyboard
