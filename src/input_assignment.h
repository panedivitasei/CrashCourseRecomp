// input_assignment.h: which input devices play which player.

#pragma once

namespace rex::input {
class InputSystem;
}

namespace cc1 {

// Called once the runtime is set up, before the game polls input.
void InstallInputAssignment(rex::input::InputSystem* input);

}  // namespace cc1
