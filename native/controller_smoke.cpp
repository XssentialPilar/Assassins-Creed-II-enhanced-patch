#include "controller_bridge.h"
#include "controller_prompts.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace {

struct RumbleCapture {
    XINPUT_VIBRATION motors{};
    unsigned writes{};
};

bool SDLCALL capture_rumble(void* userdata, Uint16 low_frequency, Uint16 high_frequency) {
    auto& capture = *static_cast<RumbleCapture*>(userdata);
    capture.motors = {low_frequency, high_frequency};
    ++capture.writes;
    return true;
}

SDL_JoystickID attach(Uint16 vendor, Uint16 product, RumbleCapture* rumble = nullptr) {
    SDL_VirtualJoystickDesc definition{};
    SDL_INIT_INTERFACE(&definition);
    definition.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    definition.vendor_id = vendor;
    definition.product_id = product;
    definition.naxes = SDL_GAMEPAD_AXIS_COUNT;
    definition.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    if (rumble) {
        definition.userdata = rumble;
        definition.Rumble = capture_rumble;
    }
    return SDL_AttachVirtualJoystick(&definition);
}

bool test_rumble(DWORD first, RumbleCapture& first_pad, DWORD second, RumbleCapture& second_pad) {
    XINPUT_CAPABILITIES capabilities{};
    const DWORD gamepad_slots[] = {first, second};
    for (DWORD slot : gamepad_slots) {
        if (controller_capabilities(slot, 0, &capabilities) != ERROR_SUCCESS ||
            !(capabilities.Flags & XINPUT_CAPS_FFB_SUPPORTED) ||
            capabilities.Vibration.wLeftMotorSpeed != 65535 || capabilities.Vibration.wRightMotorSpeed != 65535) {
            return false;
        }
    }
    constexpr XINPUT_VIBRATION low{54000, 0}, high{0, 17000}, stop{};
    if (controller_vibration(first, &low) != ERROR_SUCCESS ||
        controller_vibration(second, &high) != ERROR_SUCCESS) {
        return false;
    }
    XINPUT_STATE state{};
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 6250) {
        if (controller_state(first, &state) != ERROR_SUCCESS || controller_state(second, &state) != ERROR_SUCCESS ||
            first_pad.motors.wLeftMotorSpeed != low.wLeftMotorSpeed || first_pad.motors.wRightMotorSpeed != 0 ||
            second_pad.motors.wLeftMotorSpeed != 0 || second_pad.motors.wRightMotorSpeed != high.wRightMotorSpeed) {
            return false;
        }
        Sleep(25);
    }
    return first_pad.writes > 1 && second_pad.writes > 1 &&
           controller_vibration(first, &stop) == ERROR_SUCCESS && first_pad.motors.wLeftMotorSpeed == 0 &&
           second_pad.motors.wRightMotorSpeed == high.wRightMotorSpeed &&
           controller_vibration(second, &stop) == ERROR_SUCCESS && second_pad.motors.wRightMotorSpeed == 0;
}

bool press_south(SDL_Joystick* joystick, DWORD slot, ControllerFamily family) {
    XINPUT_STATE state{};
    return SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true) &&
           controller_state(slot, &state) == ERROR_SUCCESS &&
           state.Gamepad.wButtons == XINPUT_GAMEPAD_A &&
           active_controller_family() == family;
}

bool test_dualshock3_and_dualsense() {
    RumbleCapture ds3_rumble, dualsense_rumble;
    const auto ds3 = attach(0x054c, 0x0268, &ds3_rumble);
    const auto dualsense = attach(0x054c, 0x0ce6, &dualsense_rumble);
    auto* ds3_pad = ds3 ? SDL_OpenJoystick(ds3) : nullptr;
    auto* dualsense_pad = dualsense ? SDL_OpenJoystick(dualsense) : nullptr;
    const bool passed = ds3_pad && dualsense_pad &&
        bind_controller(2, 0x0268054c) && bind_controller(3, 0x0ce6054c) &&
        press_south(ds3_pad, 2, ControllerFamily::PS3) && press_south(dualsense_pad, 3, ControllerFamily::PS5) &&
        test_rumble(2, ds3_rumble, 3, dualsense_rumble);
    if (ds3_pad) SDL_CloseJoystick(ds3_pad);
    if (dualsense_pad) SDL_CloseJoystick(dualsense_pad);
    if (ds3) SDL_DetachVirtualJoystick(ds3);
    if (dualsense) SDL_DetachVirtualJoystick(dualsense);
    XINPUT_STATE state{};
    return passed && controller_state(2, &state) == ERROR_DEVICE_NOT_CONNECTED &&
           controller_state(3, &state) == ERROR_DEVICE_NOT_CONNECTED &&
           active_controller_family() == ControllerFamily::Unknown;
}

bool texture_digest(IDirect3DTexture9* texture, DWORD& digest) {
    if (!texture) {
        return false;
    }
    D3DLOCKED_RECT lock{};
    if (FAILED(texture->LockRect(0, &lock, nullptr, D3DLOCK_READONLY))) {
        return false;
    }
    DWORD signature = 2166136261u;
    bool has_ink = false;
    for (int y = 4; y < 28; ++y) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lock.pBits) + y * lock.Pitch);
        for (int x = 4; x < 28; ++x) {
            signature = (signature ^ row[x]) * 16777619u;
            has_ink |= x > 8 && x < 24 && y > 7 && y < 25 && (row[x] & 0xffffff) != 0;
        }
    }
    texture->UnlockRect(0);
    digest = signature;
    return has_ink;
}

bool different_glyphs() {
    HWND window = CreateWindowExW(0, L"STATIC", L"Controller glyph smoke test", WS_POPUP,
                                  0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    if (!window) {
        return false;
    }
    auto* factory = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.BackBufferWidth = parameters.BackBufferHeight = 64;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    const auto create_device = [&](D3DDEVTYPE type) {
        auto config = parameters;
        return SUCCEEDED(factory->CreateDevice(D3DADAPTER_DEFAULT, type, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &config, &device));
    };
    // Texture checks can run on the null device while another program owns full-screen Direct3D.
    bool passed = factory && (create_device(D3DDEVTYPE_HAL) || create_device(D3DDEVTYPE_NULLREF));
    if (passed) {
        for (unsigned i = 1; passed && i < static_cast<unsigned>(PromptButton::Count); ++i) {
            const auto button = static_cast<PromptButton>(i);
            const auto digest = [device, button, &passed](ControllerFamily family) {
                auto* texture = create_prompt_texture(device, family, button);
                DWORD signature = 0;
                passed &= texture_digest(texture, signature);
                if (texture) texture->Release();
                return signature;
            };
            const DWORD xbox = digest(ControllerFamily::Xbox);
            const DWORD ps3 = digest(ControllerFamily::PS3);
            const DWORD ps4 = digest(ControllerFamily::PS4);
            const DWORD ps5 = digest(ControllerFamily::PS5);
            passed &= xbox != ps3 && xbox != ps4 && xbox != ps5;
            if (button == PromptButton::Start) {
                passed &= ps3 != ps4 && ps4 == ps5;
            } else if (button == PromptButton::Back) {
                passed &= ps3 != ps4 && ps3 != ps5 && ps4 != ps5;
            } else {
                passed &= ps3 == ps4 && ps4 == ps5;
            }
        }
        device->Release();
    }
    if (factory) factory->Release();
    DestroyWindow(window);
    return passed;
}

}  // namespace

int main() {
    if (!initialize_controller_input()) {
        return 1;
    }
    RumbleCapture ps4_rumble, xbox_rumble;
    const auto ds4 = attach(0x054c, 0x09cc, &ps4_rumble);
    const auto xbox = attach(0x045e, 0x028e, &xbox_rumble);
    if (!ds4 || !xbox || !bind_controller(0, 0x09cc054c) || !bind_controller(1, 0x028e045e)) {
        std::puts(SDL_GetError());
        return 2;
    }
    auto* ps_pad = SDL_OpenJoystick(ds4);
    auto* xb_pad = SDL_OpenJoystick(xbox);
    const bool covers_both = ps_pad && xb_pad && press_south(ps_pad, 0, ControllerFamily::PS4) &&
                             press_south(xb_pad, 1, ControllerFamily::Xbox);
    const bool rumble = covers_both && test_rumble(0, ps4_rumble, 1, xbox_rumble);
    XINPUT_STATE state{};
    const bool ignores_drift = covers_both && SDL_SetJoystickVirtualAxis(ps_pad, SDL_GAMEPAD_AXIS_LEFTX, 1500) &&
        controller_state(0, &state) == ERROR_SUCCESS && active_controller_family() == ControllerFamily::Xbox;
    XINPUT_STATE unclassified{};
    unclassified.Gamepad.wButtons = XINPUT_GAMEPAD_Y;
    observe_native_controller(2, unclassified);
    const bool fallback_clears_family = active_controller_family() == ControllerFamily::Unknown;
    const bool switching_back = covers_both && SDL_SetJoystickVirtualButton(xb_pad, SDL_GAMEPAD_BUTTON_SOUTH, false) &&
        controller_state(1, &state) == ERROR_SUCCESS && press_south(xb_pad, 1, ControllerFamily::Xbox);
    SDL_CloseJoystick(ps_pad);
    SDL_CloseJoystick(xb_pad);
    SDL_DetachVirtualJoystick(xbox);
    const bool unplugged = controller_state(1, &state) == ERROR_DEVICE_NOT_CONNECTED &&
                           active_controller_family() == ControllerFamily::Unknown;
    SDL_DetachVirtualJoystick(ds4);
    const auto replacement = attach(0x054c, 0x05c4);
    auto* new_pad = replacement ? SDL_OpenJoystick(replacement) : nullptr;
    const bool rebound = new_pad && bind_controller(0, 0x05c4054c) && press_south(new_pad, 0, ControllerFamily::PS4);
    const XINPUT_VIBRATION unavailable{300, 500};
    XINPUT_CAPABILITIES capabilities{};
    const bool unavailable_rumble = rebound && controller_capabilities(0, 0, &capabilities) == ERROR_SUCCESS &&
        !(capabilities.Flags & XINPUT_CAPS_FFB_SUPPORTED) &&
        controller_vibration(0, &unavailable) == ERROR_NOT_SUPPORTED;
    if (new_pad) SDL_CloseJoystick(new_pad);
    if (replacement) SDL_DetachVirtualJoystick(replacement);
    if (!covers_both || !rumble || !unavailable_rumble || !ignores_drift || !fallback_clears_family ||
        !switching_back || !unplugged || !rebound ||
        controller_state(3, &state) != ERROR_NOT_FOUND ||
        prompt_button(0x718b3bfd74451ca5ull) != PromptButton::South ||
        prompt_button(0x28085c42e2f616beull) != PromptButton::East) {
        return 3;
    }
    if (!test_dualshock3_and_dualsense()) {
        std::puts("DualShock 3 / DualSense input, identity, or rumble check failed.");
        return 5;
    }
    std::puts("DualShock 3, DualShock 4, DualSense, and Xbox virtual input and sustained rumble passed.");
    if (!different_glyphs()) {
        std::puts("Direct3D controller glyph check failed.");
        return 4;
    }
    std::puts("Controller glyph check passed for all four families.");
    return 0;
}
