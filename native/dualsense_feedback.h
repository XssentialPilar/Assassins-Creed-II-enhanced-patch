#pragma once

#include <array>
#include <SDL3/SDL.h>

#include "controller_bridge.h"

using DualSenseTriggerReport = std::array<Uint8, 47>;
using DualSenseWirelessHapticReport = std::array<Uint8, 142>;

struct DualSenseTriggerForces {
    Uint8 left{}, right{};
    bool operator==(const DualSenseTriggerForces&) const = default;
};

DualSenseTriggerForces dualsense_trigger_forces(const XINPUT_GAMEPAD& input, const XINPUT_VIBRATION& vibration);
DualSenseTriggerReport dualsense_trigger_report(DualSenseTriggerForces forces);
DualSenseWirelessHapticReport dualsense_wireless_haptic_report(
    const std::array<std::int8_t, 64>& samples, Uint8 counter);

class DualSenseHapticWave {
public:
    explicit DualSenseHapticWave(unsigned sample_rate);
    std::array<float, 4> next(const XINPUT_VIBRATION& vibration);

private:
    double phase_[2]{}, step_[2]{};
    float amplitude_[2]{}, envelope_step_{};
};

struct DualSenseHaptics;
DualSenseHaptics* open_dualsense_haptics(SDL_Gamepad* pad, bool wireless_enabled);
bool write_dualsense_haptics(DualSenseHaptics* output, const XINPUT_VIBRATION& vibration);
void close_dualsense_haptics(DualSenseHaptics* output);
