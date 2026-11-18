#include "dualsense_feedback.h"

#include <audioclient.h>
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <numbers>
#include <string>

DualSenseTriggerForces dualsense_trigger_forces(const XINPUT_GAMEPAD& input, const XINPUT_VIBRATION& vibration) {
    const auto resistance = [](BYTE trigger, WORD motor, WORD other_motor) -> Uint8 {
        const unsigned strength = std::max<unsigned>(motor, other_motor / 2);
        if (trigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD || strength < 4096) return 0;
        // Quantize changes so the HID output queue stays quiet during a steady pull.
        return static_cast<Uint8>(12 + (strength / 4096) * 2);
    };
    return {resistance(input.bLeftTrigger, vibration.wLeftMotorSpeed, vibration.wRightMotorSpeed),
            resistance(input.bRightTrigger, vibration.wRightMotorSpeed, vibration.wLeftMotorSpeed)};
}

DualSenseTriggerReport dualsense_trigger_report(DualSenseTriggerForces forces) {
    DualSenseTriggerReport report{};
    report[0] = 0x0c;  // Update both triggers in the same HID report.
    report[10] = forces.right ? 0x01 : 0x05;
    report[12] = forces.right;
    report[21] = forces.left ? 0x01 : 0x05;
    report[23] = forces.left;
    return report;
}

DualSenseWirelessHapticReport dualsense_wireless_haptic_report(
    const std::array<std::int8_t, 64>& samples, Uint8 counter) {
    // Bluetooth output report 0x32, with one 32-frame stereo PCM block at 3 kHz.
    DualSenseWirelessHapticReport packet{0x32, static_cast<Uint8>((counter & 15) << 4),
        0x91, 7, 0xfe, 0, 0, 0, 0, 0xff, counter, 0x92, 64};
    std::memcpy(packet.data() + 13, samples.data(), samples.size());
    // The controller includes the HIDP output prefix in the checksum.
    const Uint8 prefix = 0xa2;
    Uint32 crc = SDL_crc32(0, &prefix, 1);
    crc = SDL_crc32(crc, packet.data(), packet.size() - 4);
    for (unsigned byte = 0; byte < 4; ++byte) {
        packet[packet.size() - 4 + byte] = static_cast<Uint8>(crc >> (8 * byte));
    }
    return packet;
}

DualSenseHapticWave::DualSenseHapticWave(unsigned sample_rate)
    : step_{2 * std::numbers::pi * 72 / sample_rate, 2 * std::numbers::pi * 145 / sample_rate},
      envelope_step_(1000.0f / (8 * sample_rate)) {}

std::array<float, 4> DualSenseHapticWave::next(const XINPUT_VIBRATION& vibration) {
    constexpr float gain = 0.32f / 65535;
    const float target[] = {gain * vibration.wLeftMotorSpeed, gain * vibration.wRightMotorSpeed};
    std::array<float, 4> frame{};
    for (unsigned motor = 0; motor < 2; ++motor) {
        amplitude_[motor] += std::clamp(target[motor] - amplitude_[motor], -envelope_step_, envelope_step_);
        frame[motor + 2] = static_cast<float>(std::sin(phase_[motor])) * amplitude_[motor];
        phase_[motor] += step_[motor];
        if (phase_[motor] >= 2 * std::numbers::pi) phase_[motor] -= 2 * std::numbers::pi;
    }
    return frame;
}

struct DualSenseHaptics {
    explicit DualSenseHaptics(std::string path, bool bluetooth) : hid_path(std::move(path)), wireless(bluetooth) {}
    std::string hid_path;
    bool wireless;
    std::atomic<unsigned> references{2};
    std::atomic<DWORD> motors{};
    std::atomic<bool> ready{}, stopping{};

    void release() {
        if (references.fetch_sub(1) == 1) delete this;
    }
};

namespace {

class FocusedMotors {
public:
    DWORD current(DualSenseHaptics& output) {
        const HWND foreground = GetForegroundWindow();
        if (foreground != last_foreground_) {
            last_foreground_ = foreground;
            DWORD pid = 0;
            GetWindowThreadProcessId(foreground, &pid);
            focused_ = pid == GetCurrentProcessId();
        }
        if (!focused_) output.motors = 0;
        return focused_ ? output.motors.load() : 0;
    }

private:
    HWND last_foreground_{};
    bool focused_{};
};

template<typename T>
struct ReleaseCom {
    void operator()(T* value) const { value->Release(); }
};

template<typename T>
using ComPtr = std::unique_ptr<T, ReleaseCom<T>>;

bool device_container(const wchar_t* path, GUID& container) {
    wchar_t id[MAX_DEVICE_ID_LEN]{};
    DEVPROPTYPE type{};
    ULONG bytes = sizeof(id);
    if (CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_Device_InstanceId, &type,
            reinterpret_cast<PBYTE>(id), &bytes, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING) return false;
    DEVINST devnode{};
    bytes = sizeof(container);
    return CM_Locate_DevNodeW(&devnode, id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
        CM_Get_DevNode_PropertyW(devnode, &DEVPKEY_Device_ContainerId, &type,
            reinterpret_cast<PBYTE>(&container), &bytes, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_GUID && bytes == sizeof(container) && container != GUID{};
}

ComPtr<IMMDevice> haptic_endpoint(const std::wstring& path) {
    GUID container{};
    if (!device_container(path.c_str(), container)) return {};
    IMMDeviceEnumerator* raw_enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&raw_enumerator)))) return {};
    const ComPtr<IMMDeviceEnumerator> enumerator(raw_enumerator);
    IMMDeviceCollection* raw_devices = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &raw_devices))) return {};
    const ComPtr<IMMDeviceCollection> devices(raw_devices);
    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) return {};
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* raw_device = nullptr;
        if (FAILED(devices->Item(index, &raw_device))) continue;
        ComPtr<IMMDevice> device(raw_device);
        IPropertyStore* raw_properties = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &raw_properties))) continue;
        const ComPtr<IPropertyStore> properties(raw_properties);
        PROPVARIANT property{};
        const bool matches = SUCCEEDED(properties->GetValue(PKEY_Device_ContainerId, &property)) &&
            property.vt == VT_CLSID && property.puuid && *property.puuid == container;
        PropVariantClear(&property);
        if (matches) return device;
    }
    return {};
}

enum class SampleFormat { Unsupported, Float32, Pcm16, Pcm32 };

SampleFormat sample_format(const WAVEFORMATEX& format) {
    if (format.nChannels != 4 || format.nSamplesPerSec < 32000 || format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) ||
        format.nBlockAlign != 4 * (format.wBitsPerSample / 8)) return SampleFormat::Unsupported;
    const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    constexpr DWORD QuadRear = 0x33, QuadSide = 0x603;
    if (extended.dwChannelMask != QuadRear && extended.dwChannelMask != QuadSide) return SampleFormat::Unsupported;
    constexpr GUID float_format{WAVE_FORMAT_IEEE_FLOAT, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    constexpr GUID pcm_format{WAVE_FORMAT_PCM, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    if (extended.SubFormat == float_format && format.wBitsPerSample == 32) return SampleFormat::Float32;
    if (extended.SubFormat == pcm_format && format.wBitsPerSample == 16) return SampleFormat::Pcm16;
    if (extended.SubFormat == pcm_format && format.wBitsPerSample == 32) return SampleFormat::Pcm32;
    return SampleFormat::Unsupported;
}

void fill_samples(BYTE* data, UINT32 frames, SampleFormat format, DualSenseHapticWave& wave,
                  const XINPUT_VIBRATION& vibration) {
    for (UINT32 i = 0; i < frames; ++i) {
        const auto samples = wave.next(vibration);
        for (float sample : samples) {
            if (format == SampleFormat::Pcm16) {
                *reinterpret_cast<std::int16_t*>(data) = static_cast<std::int16_t>(sample * 32767);
                data += sizeof(std::int16_t);
            } else if (format == SampleFormat::Pcm32) {
                *reinterpret_cast<std::int32_t*>(data) = static_cast<std::int32_t>(sample * 2147483647.0);
                data += sizeof(std::int32_t);
            } else {
                *reinterpret_cast<float*>(data) = sample;
                data += sizeof(float);
            }
        }
    }
}

void play_usb_haptics(DualSenseHaptics& output) {
    const char* path = output.hid_path.c_str();
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (length <= 1) return;
    std::wstring wide(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), length)) return;
    const auto device = haptic_endpoint(wide);
    IAudioClient* raw_client = nullptr;
    if (!device || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                          reinterpret_cast<void**>(&raw_client)))) return;
    const ComPtr<IAudioClient> client(raw_client);
    WAVEFORMATEX* raw_format = nullptr;
    if (FAILED(client->GetMixFormat(&raw_format))) return;
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw_format, CoTaskMemFree);
    const auto encoding = sample_format(*format);
    constexpr REFERENCE_TIME BufferDuration = 400000;  // 40 ms in 100 ns units.
    if (encoding == SampleFormat::Unsupported ||
        FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST, BufferDuration, 0,
                                  format.get(), nullptr))) return;
    IAudioRenderClient* raw_render = nullptr;
    UINT32 capacity = 0;
    if (FAILED(client->GetBufferSize(&capacity)) || FAILED(client->GetService(IID_PPV_ARGS(&raw_render)))) return;
    const ComPtr<IAudioRenderClient> render(raw_render);
    const UINT32 queued = std::min<UINT32>(capacity, format->nSamplesPerSec / 40);
    DualSenseHapticWave wave(format->nSamplesPerSec);
    bool playing = false;
    DWORD last_active = GetTickCount();
    FocusedMotors motors;
    output.ready = true;
    while (!output.stopping) {
        const DWORD levels = motors.current(output);
        const DWORD now = GetTickCount();
        if (levels) last_active = now;
        if (playing && now - last_active >= 100) {
            if (FAILED(client->Stop()) || FAILED(client->Reset())) break;
            playing = false;
        }
        if (playing || levels) {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            if (padding < queued) {
                BYTE* data = nullptr;
                const UINT32 frames = queued - padding;
                if (FAILED(render->GetBuffer(frames, &data))) break;
                fill_samples(data, frames, encoding, wave, {LOWORD(levels), HIWORD(levels)});
                if (FAILED(render->ReleaseBuffer(frames, 0))) break;
            }
            if (!playing) {
                if (FAILED(client->Start())) break;
                playing = true;
            }
        }
        SDL_Delay(playing ? 4 : 12);
    }
    output.ready = false;
    if (playing) client->Stop();
}

void play_wireless_haptics(DualSenseHaptics& output) {
    const std::unique_ptr<SDL_hid_device, decltype(&SDL_hid_close)> device(
        SDL_hid_open_path(output.hid_path.c_str()), SDL_hid_close);
    if (!device) return;
    constexpr Uint64 PeriodNs = 32000000000ull / 3000;
    DualSenseHapticWave wave(3000);
    FocusedMotors motors;
    Uint8 counter = 0;
    const auto send = [&](const std::array<std::int8_t, 64>& pcm) {
        const auto report = dualsense_wireless_haptic_report(pcm, counter++);
        return SDL_hid_write(device.get(), report.data(), report.size()) >= static_cast<int>(report.size());
    };
    if (output.stopping || !send({})) return;
    output.ready = true;
    unsigned trailing_blocks = 0;
    Uint64 deadline = SDL_GetTicksNS();
    while (!output.stopping) {
        const DWORD strength = motors.current(output);
        if (strength) trailing_blocks = 4;
        if (!trailing_blocks) {
            SDL_Delay(8);
            deadline = SDL_GetTicksNS();
            continue;
        }
        std::array<std::int8_t, 64> pcm;
        for (unsigned index = 0; index < pcm.size(); index += 2) {
            const auto frame = wave.next({LOWORD(strength), HIWORD(strength)});
            pcm[index] = static_cast<std::int8_t>(std::lround(frame[2] * 127));
            pcm[index + 1] = static_cast<std::int8_t>(std::lround(frame[3] * 127));
        }
        if (!send(pcm)) break;
        if (!strength) --trailing_blocks;
        const Uint64 now = SDL_GetTicksNS();
        deadline = std::max(deadline + PeriodNs, now);
        if (deadline > now) SDL_DelayNS(deadline - now);
    }
    output.ready = false;
    if (output.stopping && trailing_blocks) send({});
}

int SDLCALL run_haptics(void* userdata) {
    auto* output = static_cast<DualSenseHaptics*>(userdata);
    if (output->wireless) {
        play_wireless_haptics(*output);
    } else if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        play_usb_haptics(*output);
        CoUninitialize();
    }
    output->release();
    return 0;
}

}  // namespace

DualSenseHaptics* open_dualsense_haptics(SDL_Gamepad* pad, bool wireless_enabled) {
    const char* path = SDL_GetGamepadPath(pad);
    const auto connection = SDL_GetGamepadConnectionState(pad);
    const bool wireless = connection == SDL_JOYSTICK_CONNECTION_WIRELESS;
    if (!path || std::strncmp(path, "\\\\?\\", 4) || (wireless && !wireless_enabled) ||
        (!wireless && connection != SDL_JOYSTICK_CONNECTION_WIRED)) {
        return nullptr;
    }
    auto* output = new (std::nothrow) DualSenseHaptics(path, wireless);
    if (!output) return nullptr;
    auto* worker = SDL_CreateThread(run_haptics, "AC2 DualSense haptics", output);
    if (!worker) {
        delete output;
        return nullptr;
    }
    SDL_DetachThread(worker);
    return output;
}

bool write_dualsense_haptics(DualSenseHaptics* output, const XINPUT_VIBRATION& vibration) {
    if (!output) return false;
    output->motors = MAKELONG(vibration.wLeftMotorSpeed, vibration.wRightMotorSpeed);
    return output->ready;
}

void close_dualsense_haptics(DualSenseHaptics* output) {
    if (!output) return;
    output->stopping = true;
    output->release();
}
