#include "controller_bridge.h"
#include "controller_prompts.h"
#include "dualsense_feedback.h"
#include "keyboard_prompts.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cmath>
#include <cstring>

namespace {

struct RumbleCapture {
    XINPUT_VIBRATION motors{};
    unsigned writes{};
    DualSenseTriggerReport triggers{};
    unsigned trigger_writes{};
    DWORD light{0xffffffff};
};

bool SDLCALL capture_rumble(void* userdata, Uint16 low_frequency, Uint16 high_frequency) {
    auto& capture = *static_cast<RumbleCapture*>(userdata);
    capture.motors = {low_frequency, high_frequency};
    ++capture.writes;
    return true;
}

bool SDLCALL capture_effect(void* userdata, const void* data, int size) {
    auto& capture = *static_cast<RumbleCapture*>(userdata);
    if (size != static_cast<int>(capture.triggers.size())) return false;
    std::memcpy(capture.triggers.data(), data, capture.triggers.size());
    ++capture.trigger_writes;
    return true;
}

bool SDLCALL capture_light(void* userdata, Uint8 red, Uint8 green, Uint8 blue) {
    auto& capture = *static_cast<RumbleCapture*>(userdata);
    capture.light = (static_cast<DWORD>(red) << 16) | (static_cast<DWORD>(green) << 8) | blue;
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
        definition.SendEffect = capture_effect;
        definition.SetLED = capture_light;
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
    if (!SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true) ||
        controller_state(slot, &state) != ERROR_SUCCESS || state.Gamepad.wButtons != XINPUT_GAMEPAD_A) return false;
    const auto prompt = active_controller_prompt();
    return prompt.family == family && controller_prompt_pressed(PromptButton::South, prompt.pressed) &&
        !controller_prompt_pressed(PromptButton::East, prompt.pressed);
}

bool test_dualshock3_and_dualsense() {
    RumbleCapture ds3_rumble, dualsense_rumble;
    const auto ds3 = attach(0x054c, 0x0268, &ds3_rumble);
    const auto dualsense = attach(0x054c, 0x0ce6, &dualsense_rumble);
    auto* ds3_pad = ds3 ? SDL_OpenJoystick(ds3) : nullptr;
    auto* dualsense_pad = dualsense ? SDL_OpenJoystick(dualsense) : nullptr;
    const bool standard_rumble = ds3_pad && dualsense_pad &&
        bind_controller(2, 0x0268054c) && bind_controller(3, 0x0ce6054c) &&
        press_south(ds3_pad, 2, ControllerFamily::PS3) && press_south(dualsense_pad, 3, ControllerFamily::PS5) &&
        test_rumble(2, ds3_rumble, 3, dualsense_rumble);
    constexpr XINPUT_VIBRATION impact{0, 45000}, stop{};
    constexpr auto right = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    XINPUT_STATE state{};
    const bool adaptive = standard_rumble &&
        SDL_SetJoystickVirtualAxis(dualsense_pad, right, 32767) && controller_state(3, &state) == ERROR_SUCCESS &&
        controller_prompt_pressed(PromptButton::RightTrigger, active_controller_prompt().pressed) &&
        controller_vibration(3, &impact) == ERROR_SUCCESS &&
        dualsense_rumble.triggers == dualsense_trigger_report({0, 32}) &&
        SDL_SetJoystickVirtualAxis(dualsense_pad, right, -32768) && controller_state(3, &state) == ERROR_SUCCESS &&
        !controller_prompt_pressed(PromptButton::RightTrigger, active_controller_prompt().pressed) &&
        dualsense_rumble.triggers == dualsense_trigger_report({}) && ds3_rumble.trigger_writes == 0 &&
        SDL_SetJoystickVirtualAxis(dualsense_pad, right, 32767) && controller_state(3, &state) == ERROR_SUCCESS &&
        dualsense_rumble.triggers == dualsense_trigger_report({0, 32});
    controller_feedback_focus(false);
    const bool cleared = dualsense_rumble.triggers == dualsense_trigger_report({}) && dualsense_rumble.light == 0 &&
        active_controller_prompt().pressed == 0 &&
        dualsense_rumble.motors.wRightMotorSpeed == 0 && controller_vibration(3, &impact) == ERROR_SUCCESS &&
        dualsense_rumble.motors.wRightMotorSpeed == 0;
    controller_feedback_focus(true);
    const bool focused_again = controller_vibration(3, &impact) == ERROR_SUCCESS &&
        dualsense_rumble.triggers == dualsense_trigger_report({0, 32}) &&
        controller_vibration(3, &stop) == ERROR_SUCCESS && dualsense_rumble.triggers == dualsense_trigger_report({});
    const XINPUT_VIBRATION strong{60000, 0};
    const bool color_change = controller_state(3, &state) == ERROR_SUCCESS && dualsense_rumble.light == 0x847e73 &&
        controller_vibration(3, &strong) == ERROR_SUCCESS && dualsense_rumble.light == 0x9b182b &&
        controller_vibration(3, &stop) == ERROR_SUCCESS && dualsense_rumble.light == 0x9b182b;
    SDL_Delay(420);
    const bool color_restored = controller_state(3, &state) == ERROR_SUCCESS &&
        dualsense_rumble.light == 0x847e73 && ds3_rumble.light == 0xffffffff;
    if (ds3_pad) SDL_CloseJoystick(ds3_pad);
    if (dualsense_pad) SDL_CloseJoystick(dualsense_pad);
    if (ds3) SDL_DetachVirtualJoystick(ds3);
    if (dualsense) SDL_DetachVirtualJoystick(dualsense);
    return adaptive && cleared && focused_again && color_change && color_restored &&
           controller_state(2, &state) == ERROR_DEVICE_NOT_CONNECTED &&
           controller_state(3, &state) == ERROR_DEVICE_NOT_CONNECTED &&
           active_controller_family() == ControllerFamily::Unknown;
}

bool test_haptic_samples() {
    DualSenseHapticWave wave(48000);
    float peak_left = 0, peak_right = 0;
    for (int frame = 0; frame < 960; ++frame) {
        const auto samples = wave.next({65535, 32768});
        if (samples[0] || samples[1]) return false;
        peak_left = std::max(peak_left, std::abs(samples[2]));
        peak_right = std::max(peak_right, std::abs(samples[3]));
    }
    for (int frame = 0; frame < 480; ++frame) wave.next({});
    const auto report = dualsense_wireless_haptic_report({41, -34}, 3);
    return peak_left > 0.31f && peak_left <= 0.32f && peak_right > 0.15f && peak_right < 0.17f &&
        wave.next({}) == std::array<float, 4>{} && report[0] == 0x32 && report[1] == 0x30 &&
        report[11] == 0x92 && report[12] == 64 && report[13] == 41 && report[14] == 222 &&
        report[138] == 0x46 && report[139] == 0xf3 && report[140] == 0x85 && report[141] == 0x8d;
}

bool test_keyboard_profile() {
    wchar_t directory[MAX_PATH]{}, file[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"ac2", 0, file)) return false;
    const auto write = [&](const wchar_t* section, const wchar_t* field, const wchar_t* value) {
        return WritePrivateProfileStringW(section, field, value, file) != FALSE;
    };
    KeyBindings bindings;
    const auto get = [&](PromptButton role) { return bindings[static_cast<unsigned>(role)]; };
    const bool passed = write(L"Input", L"SelectedInput", L"KeyboardMouse2") &&
        write(L"KeyboardMouse2", L"Button1", L"57") && write(L"KeyboardMouse2", L"Button2", L"256") &&
        write(L"KeyboardMouse2", L"Button4", L"42") && write(L"KeyboardMouse2", L"Start", L"512") &&
        write(L"KeyboardMouse2", L"LeftStickUp", L"17") && write(L"KeyboardMouse2", L"LeftStickDown", L"31") &&
        write(L"KeyboardMouse2", L"RightStickUp", L"200") && write(L"KeyboardMouse2", L"PadLeft", L"2") &&
        write(L"KeyboardMouse2", L"StickRight", L"46") && write(L"KeyboardMouse2", L"StickLeft", L"10") &&
        write(L"KeyboardMouse5", L"Button1", L"259") && read_keyboard_profile(file, bindings) &&
        input_label(get(PromptButton::South)) == L"Space" && input_label(get(PromptButton::West)) == L"LMB" &&
        input_label(get(PromptButton::East)) == L"LShift" && get(PromptButton::Start) == -1 &&
        get(PromptButton::Apply) == 57 && get(PromptButton::Cancel) == 42 &&
        get(PromptButton::MoveForward) == 17 && get(PromptButton::MoveBack) == 31 &&
        get(PromptButton::LookUp) == 200 && get(PromptButton::Medicine) == 2 &&
        get(PromptButton::CenterCamera) == 46 && get(PromptButton::FirstPerson) == 10 &&
        write(L"KeyboardMouse2", L"Button1", L"28") && read_keyboard_profile(file, bindings) &&
        input_label(get(PromptButton::South)) == L"Enter" &&
        write(L"Input", L"SelectedInput", L"KeyboardMouse5") && read_keyboard_profile(file, bindings) &&
        input_label(get(PromptButton::South)) == L"M4" &&
        write(L"Input", L"SelectedInput", L"Wireless Controller") && !read_keyboard_profile(file, bindings) &&
        get(PromptButton::South) == -1;
    DeleteFileW(file);
    return passed && keyboard_virtual_key(0x2a) == VK_LSHIFT && keyboard_virtual_key(0x36) == VK_RSHIFT &&
        keyboard_virtual_key(0x9d) == VK_RCONTROL && keyboard_virtual_key(257) == VK_RBUTTON;
}

bool test_live_keyboard_profile() {
    constexpr unsigned ManagerSlot = 0x01e3dbc0;
    auto* image = static_cast<BYTE*>(VirtualAlloc(nullptr, ManagerSlot + sizeof(DWORD),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!image) return false;
    DWORD manager[2]{}, provider[2]{};
    std::array<BYTE, 0x59c + 2 * 0x218> profiles{};
    std::array<BYTE, 24 * 0x214> mappings{};
    const auto write = [](BYTE* location, DWORD value) { std::memcpy(location, &value, sizeof(value)); };
    const auto map = [&](unsigned index, DWORD kind, DWORD code) {
        auto* row = mappings.data() + index * 0x214;
        write(row, kind);
        write(row + 8, code);
    };
    auto* first = profiles.data() + 0x59c;
    auto* second = first + 0x218;
    provider[1] = reinterpret_cast<DWORD>(profiles.data());
    manager[1] = reinterpret_cast<DWORD>(provider);
    write(image + ManagerSlot, reinterpret_cast<DWORD>(manager));
    write(first, 1);
    write(second, 1);
    std::memcpy(first + 8, L"KeyboardMouse2", sizeof(L"KeyboardMouse2"));
    std::memcpy(second + 8, L"Controller", sizeof(L"Controller"));
    write(first + 0x214, reinterpret_cast<DWORD>(mappings.data()));
    for (unsigned index = 0; index < 24; ++index) map(index, 1, index + 1);
    map(7, 1, 0x04);
    KeyBindings bindings;
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    const auto get = [&](PromptButton role) { return bindings[static_cast<unsigned>(role)]; };
    bool passed = !supports_live_keyboard_profiles(base) &&
        read_live_keyboard_profile(base, bindings) == LiveKeyboardProfile::Keyboard &&
        get(PromptButton::Sword) == 0x04 && get(PromptButton::Start) == 10 &&
        get(PromptButton::MoveForward) == 17 && get(PromptButton::Apply) == 1;
    map(7, 1, 0x08);
    map(0, 2, 7);
    map(8, 0, 0);
    passed = passed && read_live_keyboard_profile(base, bindings) == LiveKeyboardProfile::Keyboard &&
        get(PromptButton::Sword) == 0x08 && get(PromptButton::South) == 263 &&
        get(PromptButton::Apply) == 263 && get(PromptButton::Back) == -1;
    write(profiles.data() + 0x594, 1);
    passed = passed && read_live_keyboard_profile(base, bindings) == LiveKeyboardProfile::Other;
    write(profiles.data() + 0x594, 0);
    passed = passed && read_live_keyboard_profile(base, bindings) == LiveKeyboardProfile::Keyboard &&
        get(PromptButton::Sword) == 0x08;
    VirtualFree(image, 0, MEM_RELEASE);
    return passed;
}

bool texture_digest(IDirect3DTexture9* texture, DWORD& digest) {
    if (!texture) {
        return false;
    }
    D3DSURFACE_DESC description{};
    if (FAILED(texture->GetLevelDesc(0, &description))) return false;
    const int scale = description.Width / 32;
    D3DLOCKED_RECT lock{};
    if (FAILED(texture->LockRect(0, &lock, nullptr, D3DLOCK_READONLY))) {
        return false;
    }
    DWORD signature = 2166136261u;
    bool has_ink = false;
    for (int y = 4 * scale; y < 28 * scale; ++y) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lock.pBits) + y * lock.Pitch);
        for (int x = 4 * scale; x < 28 * scale; ++x) {
            signature = (signature ^ row[x]) * 16777619u;
            const DWORD rgb = row[x] & 0xffffff;
            has_ink |= x > 8 * scale && x < 24 * scale && y > 7 * scale && y < 25 * scale &&
                rgb != 0 && rgb != 0x141312 && rgb != 0xfaf9f6;
        }
    }
    texture->UnlockRect(0);
    digest = signature;
    return has_ink;
}

bool save_glyph(IDirect3DTexture9* texture, const char* directory, unsigned family, unsigned role) {
    if (!texture) return false;
    D3DSURFACE_DESC description{};
    if (FAILED(texture->GetLevelDesc(0, &description))) return false;
    char path[MAX_PATH]{};
    if (std::snprintf(path, sizeof(path), "%s/%u-%u.tga", directory, family, role) >= MAX_PATH) return false;
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    D3DLOCKED_RECT pixels{};
    bool written = SUCCEEDED(texture->LockRect(0, &pixels, nullptr, D3DLOCK_READONLY));
    if (written) {
        const BYTE header[] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            LOBYTE(description.Width), HIBYTE(description.Width), LOBYTE(description.Height),
            HIBYTE(description.Height), 32, 0x28};
        written = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
        for (int row = static_cast<int>(description.Height) - 1; row >= 0 && written; --row) {
            const auto* scan = static_cast<const BYTE*>(pixels.pBits) + row * pixels.Pitch;
            written = std::fwrite(scan, 4, description.Width, file) == description.Width;
        }
        texture->UnlockRect(0);
    }
    return std::fclose(file) == 0 && written;
}

bool test_action_atlas(IDirect3DDevice9* device) {
    IDirect3DTexture9* original = nullptr;
    if (FAILED(device->CreateTexture(128, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &original, nullptr))) {
        return false;
    }
    D3DLOCKED_RECT pixels{};
    constexpr DWORD Stock{0xff334455};
    const bool seeded = SUCCEEDED(original->LockRect(0, &pixels, nullptr, 0));
    if (seeded) {
        for (unsigned y = 0; y < 256; ++y) {
            auto* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(pixels.pBits) + y * pixels.Pitch);
            std::fill_n(row, 128, Stock);
        }
        original->UnlockRect(0);
    }
    auto* atlas = seeded ? create_action_prompt_atlas(device, original) : nullptr;
    auto* released = create_key_prompt_texture(device, 0x39);
    auto* pressed = create_key_prompt_texture(device, 0x39, true);
    const auto read = [atlas](unsigned x, unsigned y) {
        D3DLOCKED_RECT samples{};
        if (!atlas || FAILED(atlas->LockRect(0, &samples, nullptr, D3DLOCK_READONLY))) return DWORD{0};
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(samples.pBits) + y * samples.Pitch);
        const DWORD color = row[x];
        atlas->UnlockRect(0);
        return color;
    };
    const bool passed = atlas && released && pressed && read(16, 148) == Stock &&
        update_action_prompt_atlas(atlas, original, PromptButton::South, released) &&
        (read(16, 148) >> 24) == 209 && read(6, 148) == 0 && read(32, 140) == 0 &&
        read(8, 16) == Stock &&
        update_action_prompt_atlas(atlas, original, PromptButton::South, pressed) &&
        read(16, 148) == 0xfff8f5ed && read(6, 148) == 0 && read(8, 16) == Stock &&
        update_action_prompt_atlas(atlas, original, PromptButton::South, nullptr) &&
        read(16, 148) == Stock && read(8, 16) == Stock;
    if (pressed) pressed->Release();
    if (released) released->Release();
    if (atlas) atlas->Release();
    original->Release();
    return passed;
}

bool different_glyphs(const char* preview_directory = nullptr, bool complete_keyboard = false) {
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
        for (unsigned i = 1; passed && i <= static_cast<unsigned>(PromptButton::Cancel); ++i) {
            const auto button = static_cast<PromptButton>(i);
            const auto digest = [device, button, &passed, preview_directory](ControllerFamily family) {
                DWORD signature[2]{};
                for (unsigned pressed = 0; pressed < 2; ++pressed) {
                    auto* texture = create_prompt_texture(device, family, button, pressed != 0);
                    passed &= texture_digest(texture, signature[pressed]);
                    if (preview_directory) {
                        const unsigned row = static_cast<unsigned>(family) + pressed * 6;
                        passed &= save_glyph(texture, preview_directory, row, static_cast<unsigned>(button));
                    }
                    if (texture) texture->Release();
                }
                passed &= signature[0] != signature[1];
                return signature[0];
            };
            const DWORD xbox = digest(ControllerFamily::Xbox);
            const DWORD ps3 = digest(ControllerFamily::PS3);
            const DWORD ps4 = digest(ControllerFamily::PS4);
            const DWORD ps5 = digest(ControllerFamily::PS5);
            passed &= xbox != ps3 && xbox != ps4 && xbox != ps5 && ps3 != ps4 && ps3 != ps5 && ps4 != ps5;
        }
        auto* key = create_key_prompt_texture(device, 0x2a);
        auto* held_key = create_key_prompt_texture(device, 0x2a, true);
        DWORD signature = 0;
        DWORD held_signature = 0;
        passed &= texture_digest(key, signature) && texture_digest(held_key, held_signature) &&
            signature != held_signature;
        if (key) key->Release();
        if (held_key) held_key->Release();
        if (preview_directory) {
            const int bindings[] = {0x39, 0x2a, 256, 0x12, 0x21, 257, 0x14, 0x10, 1, 15,
                                    0x11, 0x1f, 0x1e, 0x20, 0xc8, 0xd0, 0xcb, 0xcd,
                                    0x02, 0x03, 0x04, 0x05, 0x2e, 0x0a};
            for (unsigned index = 0; index < std::size(bindings); ++index) {
                auto* texture = create_key_prompt_texture(device, bindings[index]);
                auto* pressed = create_key_prompt_texture(device, bindings[index], true);
                passed &= save_glyph(texture, preview_directory, 0, index + 1);
                passed &= save_glyph(pressed, preview_directory, 5, index + 1);
                if (texture) texture->Release();
                if (pressed) pressed->Release();
            }
            if (complete_keyboard) {
                for (unsigned code = 1; code <= 263; ++code) {
                    for (unsigned pressed = 0; pressed <= 1; ++pressed) {
                        auto* texture = create_key_prompt_texture(device, static_cast<int>(code), pressed != 0);
                        passed &= save_glyph(texture, preview_directory, 20 + pressed, code);
                        if (texture) texture->Release();
                    }
                }
            }
        }
        passed &= test_action_atlas(device);
        device->Release();
    }
    if (factory) factory->Release();
    DestroyWindow(window);
    return passed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--check-live-bindings") == 0)
        return test_live_keyboard_profile() && test_keyboard_profile() ? 0 : 5;
    if (argc == 3 && std::strcmp(argv[1], "--render-keyboard") == 0)
        return different_glyphs(argv[2], true) ? 0 : 4;
    if (argc == 3 && std::strcmp(argv[1], "--render-atlas") == 0) {
        return different_glyphs(argv[2]) ? 0 : 4;
    }
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
    const bool rumble = covers_both && test_rumble(0, ps4_rumble, 1, xbox_rumble) &&
        ps4_rumble.light == 0x9b182b && xbox_rumble.light == 0xffffffff;
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
        prompt_button(0x718b3bfd74451ca5ull) != PromptButton::Apply ||
        prompt_button(0x28085c42e2f616beull) != PromptButton::Cancel) {
        return 3;
    }
    if (!test_dualshock3_and_dualsense()) {
        std::puts("DualShock 3 / DualSense input, identity, or rumble check failed.");
        return 5;
    }
    std::puts("Four controller families, sustained rumble, PlayStation light changes, and DualSense trigger reports passed.");
    if (!test_haptic_samples() || !test_keyboard_profile() || !test_live_keyboard_profile()) {
        std::puts("Haptic channel or keyboard profile check failed.");
        return 6;
    }
    std::puts("Haptic channel isolation, Bluetooth report checksum, and live keyboard binding file changes passed.");
    if (!different_glyphs()) {
        std::puts("Direct3D controller glyph check failed.");
        return 4;
    }
    std::puts("Controller glyph check passed for all four families.");
    return 0;
}
