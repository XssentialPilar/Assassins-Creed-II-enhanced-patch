#pragma once

#include <cstdint>
#include <d3d9.h>

#include "controller_bridge.h"

enum class PromptButton : BYTE {
    None, South, East, West, North, LeftTrigger, RightTrigger, LeftShoulder, RightShoulder, Start, Back, Count
};

PromptButton prompt_button(std::uint64_t fingerprint);
IDirect3DTexture9* create_prompt_texture(IDirect3DDevice9* device, ControllerFamily family, PromptButton button);
