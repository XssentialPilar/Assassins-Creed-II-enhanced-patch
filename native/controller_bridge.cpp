#include "controller_bridge.h"
#include "dualsense_feedback.h"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

struct Slot {
    SDL_Gamepad* pad{};
    DWORD product{}, packet{}, last_scan{};
    XINPUT_GAMEPAD last{};
    XINPUT_VIBRATION vibration{};
    DWORD rumble_update{};
    ControllerFamily family{};
    DualSenseHaptics* haptics{};
    DualSenseTriggerForces trigger_forces{};
    DWORD trigger_retry{};
    DWORD impact_until{}, light_retry{};
    DWORD light_color{0xffffffff};
    bool haptics_checked{}, using_haptics{};
};

constexpr DWORD RumbleLeaseMs = 6000;
constexpr DWORD RumbleRefreshMs = 2000;
bool wireless_haptics_enabled = true;

bool has_rumble(SDL_Gamepad* pad) {
    return SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

void update_triggers(Slot& slot, DWORD now) {
    if (slot.family != ControllerFamily::PS5) return;
    const auto forces = dualsense_trigger_forces(slot.last, slot.vibration);
    if (forces == slot.trigger_forces || (slot.trigger_retry && now - slot.trigger_retry < 1000)) return;
    const auto report = dualsense_trigger_report(forces);
    if (SDL_SendGamepadEffect(slot.pad, report.data(), static_cast<int>(report.size()))) {
        slot.trigger_forces = forces;
        slot.trigger_retry = 0;
    } else {
        slot.trigger_retry = now;
    }
}

void update_light(Slot& slot, DWORD now, bool focused) {
    if (slot.family != ControllerFamily::PS4 && slot.family != ControllerFamily::PS5) return;
    const bool impact = slot.vibration.wLeftMotorSpeed >= 48000 || slot.vibration.wRightMotorSpeed >= 48000;
    if (impact) slot.impact_until = now + 400;
    constexpr DWORD Ivory = 0x847e73, Carmine = 0x9b182b;
    if (!impact && static_cast<LONG>(slot.impact_until - now) <= 0) slot.impact_until = 0;
    const DWORD color = focused ? (slot.impact_until ? Carmine : Ivory) : 0;
    if (color == slot.light_color || (slot.light_retry && now - slot.light_retry < 1000)) return;
    const auto properties = SDL_GetGamepadProperties(slot.pad);
    if (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false) &&
        SDL_SetGamepadLED(slot.pad, static_cast<Uint8>(color >> 16), static_cast<Uint8>(color >> 8),
                         static_cast<Uint8>(color))) {
        slot.light_color = color;
        slot.light_retry = 0;
    } else {
        slot.light_retry = now;
    }
}

bool send_rumble(Slot& slot, const XINPUT_VIBRATION& vibration, DWORD now) {
    const bool active = vibration.wLeftMotorSpeed || vibration.wRightMotorSpeed;
    slot.rumble_update = now;
    if (active && slot.family == ControllerFamily::PS5 && !slot.haptics_checked) {
        slot.haptics_checked = true;
        slot.haptics = open_dualsense_haptics(slot.pad, wireless_haptics_enabled);
    }
    const bool haptics = write_dualsense_haptics(slot.haptics, vibration);
    if (haptics && !slot.using_haptics) SDL_RumbleGamepad(slot.pad, 0, 0, 0);
    slot.using_haptics = haptics;
    const bool success = haptics || SDL_RumbleGamepad(slot.pad, vibration.wLeftMotorSpeed,
        vibration.wRightMotorSpeed, active ? RumbleLeaseMs : 0);
    if (success || !active) {
        slot.vibration = vibration;
    }
    update_triggers(slot, now);
    return success;
}

std::array<Slot, XUSER_MAX_COUNT> slots;
std::array<XINPUT_GAMEPAD, XUSER_MAX_COUNT> native_states;
std::mutex state_lock;
DWORD active_slot = XUSER_MAX_COUNT;
std::atomic<DWORD> prompt_state{};
bool feedback_focused = true;

// Slot mutations are serialized by state_lock. Texture binds get a single snapshot.
void select_prompt_slot(DWORD index) {
    active_slot = index;
    DWORD state = 0;
    if (index < slots.size()) {
        const Slot& slot = slots[index];
        state = static_cast<DWORD>(slot.family) << 24;
        if (feedback_focused) {
            state |= slot.last.wButtons;
            if (slot.last.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) state |= PromptLeftTrigger;
            if (slot.last.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) state |= PromptRightTrigger;
        }
    }
    prompt_state.store(state, std::memory_order_relaxed);
}

bool new_input(const XINPUT_GAMEPAD& before, const XINPUT_GAMEPAD& after) {
    const auto trigger = [](BYTE previous, BYTE current) {
        return current > XINPUT_GAMEPAD_TRIGGER_THRESHOLD &&
               (previous <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD || current - previous >= 16);
    };
    const auto stick = [](SHORT previous, SHORT current, int deadzone) {
        return std::abs(current) > deadzone &&
               (std::abs(previous) <= deadzone || std::abs(current - previous) >= 2048);
    };
    return (after.wButtons & ~before.wButtons) ||
        trigger(before.bLeftTrigger, after.bLeftTrigger) || trigger(before.bRightTrigger, after.bRightTrigger) ||
        stick(before.sThumbLX, after.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
        stick(before.sThumbLY, after.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
        stick(before.sThumbRX, after.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) ||
        stick(before.sThumbRY, after.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
}

ControllerFamily family_of(SDL_Gamepad* pad) {
    const auto vendor = SDL_GetGamepadVendor(pad);
    const auto type = SDL_GetRealGamepadType(pad);
    if (vendor == 0x054c) {
        switch (type) {
            case SDL_GAMEPAD_TYPE_PS3: return ControllerFamily::PS3;
            case SDL_GAMEPAD_TYPE_PS4: return ControllerFamily::PS4;
            case SDL_GAMEPAD_TYPE_PS5: return ControllerFamily::PS5;
            default: break;
        }
    }
    if (vendor == 0x045e && (type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE)) {
        return ControllerFamily::Xbox;
    }
    return ControllerFamily::Unknown;
}

bool find_pad(DWORD index, DWORD product) {
    int count = 0;
    const SDL_JoystickID* ids = SDL_GetGamepads(&count);
    Slot& slot = slots[index];
    for (int i = 0; i < count; ++i) {
        const SDL_JoystickID id = ids[i];
        if (SDL_GetGamepadVendorForID(id) != LOWORD(product) ||
            SDL_GetGamepadProductForID(id) != HIWORD(product)) {
            continue;
        }
        bool assigned = false;
        for (const Slot& other : slots) {
            assigned |= other.pad && SDL_GetGamepadID(other.pad) == id;
        }
        if (assigned) {
            continue;
        }
        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad) {
            continue;
        }
        const ControllerFamily family = family_of(pad);
        if (family == ControllerFamily::Unknown) {
            SDL_CloseGamepad(pad);
            continue;
        }
        slot.pad = pad;
        slot.product = product;
        slot.family = family;
        slot.last = {};
        ++slot.packet;
        if (active_slot == XUSER_MAX_COUNT) {
            select_prompt_slot(index);
        }
        break;
    }
    SDL_free(const_cast<SDL_JoystickID*>(ids));
    return slot.pad != nullptr;
}

void release_disconnected_pad(DWORD index, Slot& slot) {
    if (slot.pad && !SDL_GamepadConnected(slot.pad)) {
        close_dualsense_haptics(slot.haptics);
        SDL_CloseGamepad(slot.pad);
        slot.pad = nullptr;
        slot.haptics = nullptr;
        slot.haptics_checked = slot.using_haptics = false;
        slot.trigger_forces = {};
        slot.trigger_retry = 0;
        slot.impact_until = slot.light_retry = 0;
        slot.light_color = 0xffffffff;
        slot.vibration = {};
        if (active_slot == index) {
            select_prompt_slot(XUSER_MAX_COUNT);
        }
    }
}

Slot* connected_slot(DWORD index) {
    if (index >= slots.size() || !slots[index].product) {
        return nullptr;
    }
    Slot& slot = slots[index];
    release_disconnected_pad(index, slot);
    const DWORD now = GetTickCount();
    if (!slot.pad && now - slot.last_scan >= 500) {
        slot.last_scan = now;
        find_pad(index, slot.product);
    }
    return &slot;
}

XINPUT_GAMEPAD read_buttons(SDL_Gamepad* pad) {
    static constexpr struct {
        SDL_GamepadButton sdl;
        WORD xbox;
    } buttons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A}, {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
        {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X}, {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
        {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK}, {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
    };
    XINPUT_GAMEPAD state{};
    for (const auto& button : buttons) {
        if (SDL_GetGamepadButton(pad, button.sdl)) {
            state.wButtons |= button.xbox;
        }
    }
    const auto axis = [pad](SDL_GamepadAxis id) { return SDL_GetGamepadAxis(pad, id); };
    const auto flip = [](Sint16 value) { return static_cast<SHORT>(value == -32768 ? 32767 : -value); };
    state.sThumbLX = axis(SDL_GAMEPAD_AXIS_LEFTX);
    state.sThumbLY = flip(axis(SDL_GAMEPAD_AXIS_LEFTY));
    state.sThumbRX = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    state.sThumbRY = flip(axis(SDL_GAMEPAD_AXIS_RIGHTY));
    state.bLeftTrigger = static_cast<BYTE>(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767);
    state.bRightTrigger = static_cast<BYTE>(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767);
    return state;
}

}  // namespace

bool initialize_controller_input() {
    wchar_t appdata[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, std::size(appdata));
    if (length && length < std::size(appdata)) {
        const auto ini = std::wstring(appdata) + L"\\Ubisoft\\Assassin's Creed 2\\AC2EnhancedPatch.ini";
        wireless_haptics_enabled = GetPrivateProfileIntW(L"Controllers", L"DualSenseBluetoothHaptics", 1, ini.c_str()) != 0;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "auto");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        return false;
    }
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
    return true;
}

bool bind_controller(DWORD index, DWORD product) {
    if (index >= slots.size() || (LOWORD(product) != 0x045e && LOWORD(product) != 0x054c)) {
        return false;
    }
    std::lock_guard lock(state_lock);
    Slot& slot = slots[index];
    SDL_UpdateGamepads();
    release_disconnected_pad(index, slot);
    if (slot.pad) {
        return slot.product == product && SDL_GamepadConnected(slot.pad);
    }
    return find_pad(index, product);
}

ControllerFamily active_controller_family() {
    return active_controller_prompt().family;
}

ControllerPromptState active_controller_prompt() {
    const DWORD state = prompt_state.load(std::memory_order_relaxed);
    return {static_cast<ControllerFamily>(state >> 24), state & 0xffffff};
}

void controller_feedback_focus(bool focused) {
    static std::atomic<bool> last_focus{true};
    if (last_focus.exchange(focused) == focused) return;
    std::lock_guard lock(state_lock);
    feedback_focused = focused;
    select_prompt_slot(active_slot);
    if (focused) return;
    const DWORD now = GetTickCount();
    for (Slot& slot : slots) {
        if (slot.pad) {
            if (slot.vibration.wLeftMotorSpeed || slot.vibration.wRightMotorSpeed ||
                slot.trigger_forces != DualSenseTriggerForces{}) {
                slot.trigger_retry = 0;
                send_rumble(slot, {}, now);
            }
            slot.impact_until = 0;
            update_light(slot, now, false);
        }
    }
}

void stop_controller_feedback() {
    std::lock_guard lock(state_lock);
    const DWORD now = GetTickCount();
    for (Slot& slot : slots) {
        if (slot.pad && (slot.vibration.wLeftMotorSpeed || slot.vibration.wRightMotorSpeed ||
                         slot.trigger_forces != DualSenseTriggerForces{})) {
            slot.trigger_retry = 0;
            send_rumble(slot, {}, now);
        }
        close_dualsense_haptics(slot.haptics);
        slot.haptics = nullptr;
        slot.haptics_checked = slot.using_haptics = false;
        if (slot.pad) update_light(slot, now, false);
    }
}

DWORD controller_state(DWORD index, XINPUT_STATE* state) {
    std::lock_guard lock(state_lock);
    if (index >= slots.size() || !slots[index].product) {
        return ERROR_NOT_FOUND;
    }
    SDL_UpdateGamepads();
    Slot* slot = connected_slot(index);
    if (!state || !slot->pad) {
        return state ? ERROR_DEVICE_NOT_CONNECTED : ERROR_BAD_ARGUMENTS;
    }
    const DWORD now = GetTickCount();
    const XINPUT_GAMEPAD current = read_buttons(slot->pad);
    if (std::memcmp(&current, &slot->last, sizeof(current)) != 0) {
        const bool engaged = new_input(slot->last, current);
        slot->last = current;
        if (engaged || active_slot == index) select_prompt_slot(index);
        ++slot->packet;
    }
    if (slot->vibration.wLeftMotorSpeed || slot->vibration.wRightMotorSpeed) {
        const bool haptics = write_dualsense_haptics(slot->haptics, slot->vibration);
        if (haptics != slot->using_haptics || (!haptics && now - slot->rumble_update >= RumbleRefreshMs)) {
            send_rumble(*slot, slot->vibration, now);
        }
    }
    update_triggers(*slot, now);
    update_light(*slot, now, feedback_focused);
    *state = {slot->packet, current};
    return ERROR_SUCCESS;
}

void observe_native_controller(DWORD index, const XINPUT_STATE& state) {
    if (index >= native_states.size()) {
        return;
    }
    std::lock_guard lock(state_lock);
    // Windows XInput exposes no physical identity on this route.
    if (new_input(native_states[index], state.Gamepad)) {
        select_prompt_slot(XUSER_MAX_COUNT);
    }
    native_states[index] = state.Gamepad;
}

DWORD controller_capabilities(DWORD index, DWORD, XINPUT_CAPABILITIES* capabilities) {
    std::lock_guard lock(state_lock);
    Slot* slot = connected_slot(index);
    if (!slot) {
        return ERROR_NOT_FOUND;
    }
    if (!capabilities || !slot->pad) {
        return capabilities ? ERROR_DEVICE_NOT_CONNECTED : ERROR_BAD_ARGUMENTS;
    }
    *capabilities = {};
    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    capabilities->Gamepad = {0xf3ff, 255, 255, 32767, 32767, 32767, 32767};
    if (has_rumble(slot->pad)) {
        capabilities->Flags |= XINPUT_CAPS_FFB_SUPPORTED;
        capabilities->Vibration = {65535, 65535};
    }
    return ERROR_SUCCESS;
}

DWORD controller_vibration(DWORD index, const XINPUT_VIBRATION* vibration) {
    std::lock_guard lock(state_lock);
    Slot* slot = connected_slot(index);
    if (!slot) {
        return ERROR_NOT_FOUND;
    }
    if (!vibration || !slot->pad) {
        return vibration ? ERROR_DEVICE_NOT_CONNECTED : ERROR_BAD_ARGUMENTS;
    }
    if (!has_rumble(slot->pad)) {
        return ERROR_NOT_SUPPORTED;
    }
    const DWORD now = GetTickCount();
    const bool sent = send_rumble(*slot, feedback_focused ? *vibration : XINPUT_VIBRATION{}, now);
    update_light(*slot, now, feedback_focused);
    return sent ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}
