#pragma once

#include <windows.h>
#include <xinput.h>

enum class ControllerFamily { Unknown, Xbox, PS4 };

bool initialize_controller_input();
bool bind_controller(DWORD slot, DWORD product);
ControllerFamily active_controller_family();
void observe_native_controller(DWORD slot, const XINPUT_STATE& state);

// ERROR_NOT_FOUND means the slot belongs to the original Windows XInput path.
DWORD controller_state(DWORD slot, XINPUT_STATE* state);
DWORD controller_capabilities(DWORD slot, DWORD flags, XINPUT_CAPABILITIES* capabilities);
DWORD controller_vibration(DWORD slot, const XINPUT_VIBRATION* vibration);
