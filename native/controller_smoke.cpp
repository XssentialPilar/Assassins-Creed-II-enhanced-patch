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

bool test_rumble(RumbleCapture& ps4, RumbleCapture& xbox) {
    XINPUT_CAPABILITIES capabilities{};
    constexpr DWORD gamepad_slots[] = {0, 1};
    for (DWORD slot : gamepad_slots) {
        if (controller_capabilities(slot, 0, &capabilities) != ERROR_SUCCESS ||
            !(capabilities.Flags & XINPUT_CAPS_FFB_SUPPORTED) ||
            capabilities.Vibration.wLeftMotorSpeed != 65535 || capabilities.Vibration.wRightMotorSpeed != 65535) {
            return false;
        }
    }
    constexpr XINPUT_VIBRATION low{54000, 0}, high{0, 17000}, stop{};
    if (controller_vibration(0, &low) != ERROR_SUCCESS || controller_vibration(1, &high) != ERROR_SUCCESS) {
        return false;
    }
    XINPUT_STATE state{};
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 6250) {
        if (controller_state(0, &state) != ERROR_SUCCESS || controller_state(1, &state) != ERROR_SUCCESS ||
            ps4.motors.wLeftMotorSpeed != low.wLeftMotorSpeed || ps4.motors.wRightMotorSpeed != 0 ||
            xbox.motors.wLeftMotorSpeed != 0 || xbox.motors.wRightMotorSpeed != high.wRightMotorSpeed) {
            return false;
        }
        Sleep(25);
    }
    return ps4.writes > 1 && xbox.writes > 1 &&
           controller_vibration(0, &stop) == ERROR_SUCCESS && ps4.motors.wLeftMotorSpeed == 0 &&
           xbox.motors.wRightMotorSpeed == high.wRightMotorSpeed &&
           controller_vibration(1, &stop) == ERROR_SUCCESS && xbox.motors.wRightMotorSpeed == 0;
}

bool press_south(SDL_Joystick* joystick, DWORD slot, ControllerFamily family) {
    XINPUT_STATE state{};
    return SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true) &&
           controller_state(slot, &state) == ERROR_SUCCESS &&
           state.Gamepad.wButtons == XINPUT_GAMEPAD_A &&
           active_controller_family() == family;
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
    bool passed = factory && SUCCEEDED(factory->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device));
    if (passed) {
        for (unsigned i = 1; passed && i < static_cast<unsigned>(PromptButton::Count); ++i) {
            const auto button = static_cast<PromptButton>(i);
            auto* ps4 = create_prompt_texture(device, ControllerFamily::PS4, button);
            auto* xbox = create_prompt_texture(device, ControllerFamily::Xbox, button);
            DWORD ps_digest = 0, xb_digest = 0;
            passed = texture_digest(ps4, ps_digest) && texture_digest(xbox, xb_digest) && ps_digest != xb_digest;
            if (ps4) ps4->Release();
            if (xbox) xbox->Release();
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
    const bool rumble = covers_both && test_rumble(ps4_rumble, xbox_rumble);
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
    if (!different_glyphs()) {
        return 4;
    }
    std::puts("PS4 and Xbox switching, distinct glyphs, reconnection, and independent sustained rumble passed.");
    return 0;
}
