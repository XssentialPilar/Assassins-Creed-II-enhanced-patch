#pragma once

#include <cstdint>
#include <d3d9.h>

#include "controller_bridge.h"

enum class PromptButton : BYTE {
    None, South, East, West, North, LeftTrigger, RightTrigger, LeftShoulder, RightShoulder, Start, Back,
    Apply, Cancel,
    MoveForward, MoveBack, MoveLeft, MoveRight, LookUp, LookDown, LookLeft, LookRight,
    Medicine, HiddenBlade, Sword, Fists, CenterCamera, FirstPerson,
    Count
};

PromptButton prompt_button(std::uint64_t fingerprint);
bool controller_prompt_pressed(PromptButton button, DWORD pressed);
IDirect3DTexture9* create_prompt_texture(IDirect3DDevice9* device, ControllerFamily family, PromptButton button,
                                       bool pressed = false);
IDirect3DTexture9* create_key_prompt_texture(IDirect3DDevice9* device, int binding, bool pressed = false,
                                           HKL layout = nullptr);
IDirect3DTexture9* create_prompt_notice_texture(IDirect3DDevice9* device, const wchar_t* label);
IDirect3DTexture9* create_action_prompt_atlas(IDirect3DDevice9* device, IDirect3DTexture9* original);
bool update_action_prompt_atlas(IDirect3DTexture9* atlas, IDirect3DTexture9* original,
                                PromptButton role, IDirect3DTexture9* replacement);
