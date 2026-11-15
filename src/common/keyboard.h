#pragma once

namespace input {
struct Pad;
}

namespace keyboard {
// port 0 only: keys press the console pad's buttons, added to whatever a real controller reports
void Apply(int port, input::Pad& pad);
}  // namespace keyboard
