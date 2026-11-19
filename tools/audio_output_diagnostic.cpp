#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include "audio_channel_map.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>

namespace {
using namespace ac2::audio;
namespace fs = std::filesystem;

template<class T> struct Releaser { void operator()(T* p) const noexcept { p->Release(); } };
template<class T> using Com = std::unique_ptr<T, Releaser<T>>;
using MixFormat = std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)>;

constexpr GUID Pcm{1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
constexpr GUID Float{3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
constexpr float DeviceLevel = -36, SoftwareLevel = -24;
constexpr double Leading = .25, Slot = .60, ToneLength = .40, Ramp = .02;
constexpr unsigned MaxRate = 384000;

void require(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}
void checked(HRESULT result, const char* text) {
    if (SUCCEEDED(result)) return;
    char error[160];
    std::snprintf(error, sizeof(error), "%s (0x%08lx)", text, static_cast<unsigned long>(result));
    throw std::runtime_error(error);
}
unsigned number(const wchar_t* value, unsigned low, unsigned high) {
    wchar_t* end{};
    const auto parsed = std::wcstoul(value, &end, 10);
    require(*value && !*end && parsed >= low && parsed <= high, "Numeric argument outside the supported range");
    return parsed;
}
std::wstring endpoint_id(IMMDevice& device) {
    LPWSTR raw{};
    checked(device.GetId(&raw), "Get endpoint ID");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> id(raw, CoTaskMemFree);
    return id.get();
}
Com<IMMDeviceEnumerator> devices() {
    IMMDeviceEnumerator* raw{};
    checked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw)), "Enumerate Windows audio");
    return Com<IMMDeviceEnumerator>(raw);
}
Com<IMMDevice> endpoint(IMMDeviceEnumerator& enumerator, std::wstring_view id) {
    IMMDevice* raw{};
    if (id == L"default")
        checked(enumerator.GetDefaultAudioEndpoint(eRender, eConsole, &raw), "Get the default render endpoint");
    else
        checked(enumerator.GetDevice(std::wstring(id).c_str(), &raw), "Open the selected endpoint");
    Com<IMMDevice> device(raw);
    IMMEndpoint* kind{};
    checked(device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&kind)), "Get endpoint direction");
    const Com<IMMEndpoint> direction(kind);
    EDataFlow flow{};
    checked(direction->GetDataFlow(&flow), "Read endpoint direction");
    require(flow == eRender, "The endpoint is not a render device");
    return device;
}
Com<IAudioClient> audio_client(IMMDevice& device) {
    IAudioClient* raw{};
    checked(device.Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
        reinterpret_cast<void**>(&raw)), "Activate WASAPI");
    return Com<IAudioClient>(raw);
}
MixFormat shared_format(IAudioClient& client) {
    WAVEFORMATEX* raw{};
    checked(client.GetMixFormat(&raw), "Get the endpoint's shared format");
    return {raw, CoTaskMemFree};
}
void check_format(IAudioClient& client, const WAVEFORMATEX& wave, AUDCLNT_SHAREMODE mode) {
    WAVEFORMATEX* raw{};
    const HRESULT result = client.IsFormatSupported(mode, &wave, mode == AUDCLNT_SHAREMODE_SHARED ? &raw : nullptr);
    const MixFormat closest(raw, CoTaskMemFree);
    checked(result, mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? "Driver rejected the requested DirectOut format"
                                                      : "WASAPI rejected the shared endpoint format");
    require(result == S_OK, "The endpoint only offered a different stream format");
}
void request_raw_processing(IAudioClient& client) {
    IAudioClient2* raw{};
    checked(client.QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(&raw)), "Get WASAPI RAW capability");
    const Com<IAudioClient2> control(raw);
    const AudioClientProperties properties{sizeof(AudioClientProperties), FALSE, AudioCategory_GameEffects,
                                          AUDCLNT_STREAMOPTIONS_RAW};
    checked(control->SetClientProperties(&properties), "Request RAW processing");
}
std::uint32_t mask(const WAVEFORMATEX& format) {
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).dwChannelMask;
    }
    if (format.nChannels == 1) return 4;
    return format.nChannels == 2 ? 3 : 0;
}

struct SampleFormat {
    unsigned channels, rate, bytes, valid;
    bool floating;

    explicit SampleFormat(const WAVEFORMATEX& source)
        : channels(source.nChannels), rate(source.nSamplesPerSec), bytes(source.wBitsPerSample / 8u),
          valid(source.wBitsPerSample), floating(source.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        bool integer = source.wFormatTag == WAVE_FORMAT_PCM;
        if (source.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            source.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto& full = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(source);
            floating = full.SubFormat == Float;
            integer = full.SubFormat == Pcm;
            valid = full.Samples.wValidBitsPerSample;
        }
        require(channels >= 1 && channels <= MaximumOutputs && rate >= 8000 && rate <= MaxRate &&
            ((integer && bytes >= 2 && bytes <= 4 && valid >= 16 && valid <= bytes * 8) ||
             (floating && bytes == 4 && valid == 32)) && source.nBlockAlign == channels * bytes &&
            source.wBitsPerSample == bytes * 8, "Unsupported PCM format or output count");
    }
    unsigned stride() const { return channels * bytes; }
    void encode(BYTE* dest, float value) const {
        if (floating) {
            std::memcpy(dest, &value, sizeof(value));
        } else {
            const auto maximum = static_cast<double>((std::uint64_t{1} << (valid - 1)) - 1);
            const auto v = static_cast<std::uint32_t>(std::llround(value * maximum)) << (bytes * 8 - valid);
            std::memcpy(dest, &v, bytes);
        }
    }
    float decode(const BYTE* src) const {
        if (floating) {
            float value{};
            std::memcpy(&value, src, sizeof(value));
            return value;
        }
        std::uint32_t raw{};
        std::memcpy(&raw, src, bytes);
        const auto signed32 = std::bit_cast<std::int32_t>(raw << (32 - bytes * 8));
        return static_cast<float>(signed32 / 2147483648.);
    }
};

WAVEFORMATEXTENSIBLE raw_format(unsigned channels, unsigned rate, unsigned bits, bool floating = false) {
    WAVEFORMATEXTENSIBLE format{};
    const auto block = static_cast<WORD>(channels * (bits / 8));
    format.Format = {WAVE_FORMAT_EXTENSIBLE, static_cast<WORD>(channels), rate, rate * block, block,
                     static_cast<WORD>(bits), sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)};
    format.Samples.wValidBitsPerSample = static_cast<WORD>(bits);
    format.SubFormat = floating ? Float : Pcm;
    // DirectOut uses stream order. Pin and speaker identity come from an explicit map.
    format.dwChannelMask = 0;
    return format;
}
float db(float amplitude) {
    return amplitude > 0 ? 20 * std::log10(amplitude) : -std::numeric_limits<float>::infinity();
}
float amplitude(float decibels) { return std::pow(10.f, decibels / 20.f); }

OutputOrder verified_order(const ChannelMap& map) {
    const auto order = output_order(map);
    require(order.has_value(), "Logical outputs must map exactly once to each stream lane");
    return *order;
}

class ToneSequence {
    const SampleFormat& format_;
    const ChannelMap& map_;
    const OutputOrder order_;
    const float level_;
    const std::optional<unsigned> single_output_;
public:
    ToneSequence(const SampleFormat& format, const ChannelMap& map, float decibels,
                 std::optional<unsigned> single_output)
        : format_(format), map_(map), order_(verified_order(map)), level_(amplitude(decibels)),
          single_output_(single_output) {
        require(map.outputs.size() == format.channels, "Output map does not match the tone stream");
    }
    unsigned slots() const { return single_output_ ? 1u : format_.channels; }
    std::size_t frames() const { return static_cast<std::size_t>((Leading + slots() * Slot + .25) * format_.rate); }
    void write(BYTE* samples, std::size_t offset, unsigned count) const {
        std::memset(samples, 0, count * format_.stride());
        for (unsigned i = 0; i < count; ++i) {
            const double time = static_cast<double>(offset + i) / format_.rate - Leading;
            if (time < 0) continue;
            const auto slot = static_cast<unsigned>(time / Slot);
            if (slot >= slots()) continue;
            const double local = time - slot * Slot;
            if (local >= ToneLength) continue;
            const float envelope = static_cast<float>(std::clamp(std::min(local, ToneLength - local) / Ramp, 0., 1.));
            const unsigned lane = order_[single_output_.value_or(slot)];
            const auto& output = map_.outputs[lane];
            const double hz = output.speaker.starts_with("LFE") || output.bed_channel == 3u ? 63. : 437.;
            const float value = level_ * envelope *
                static_cast<float>(std::sin(2 * std::numbers::pi * hz * local));
            format_.encode(samples + i * format_.stride() + lane * format_.bytes, value);
        }
    }
};

struct Result {
    unsigned logical{}, expected{}, observed{};
    float level{}, inactive{};
    bool order_pass{}, level_pass{}, isolation_pass{};
};
using Results = std::vector<Result>;

Results measure(const SampleFormat& format, std::span<const BYTE> bytes, const OutputOrder& order, std::size_t first_frame,
                float level_db, bool exact, float tolerance_db, float isolation_db,
                std::optional<unsigned> single_output = {}) {
    Results result;
    const auto slots = single_output ? 1u : format.channels;
    for (unsigned slot = 0; slot < slots; ++slot) {
        const unsigned channel = single_output.value_or(slot);
        const double start = slot * Slot + .08;
        const double end = slot * Slot + .32;
        const auto a = first_frame + static_cast<std::size_t>(start * format.rate);
        const auto b = first_frame + static_cast<std::size_t>(end * format.rate);
        require(b <= bytes.size() / format.stride(), "Capture did not cover every test slot");
        std::array<float, MaximumOutputs> peaks{};
        bool valid = true;
        for (auto i = a; i < b; ++i) {
            for (unsigned lane = 0; lane < format.channels; ++lane) {
                const float sample = format.decode(bytes.data() + i * format.stride() + lane * format.bytes);
                if (!std::isfinite(sample)) valid = false;
                else peaks[lane] = std::max(peaks[lane], std::abs(sample));
            }
        }
        const auto end_peak = peaks.begin() + format.channels;
        const auto observed = static_cast<unsigned>(std::max_element(peaks.begin(), end_peak) - peaks.begin());
        const unsigned expected = order[channel];
        const float active = peaks[expected];
        peaks[expected] = 0;
        const float inactive = *std::max_element(peaks.begin(), end_peak);
        result.push_back({channel + 1, expected + 1, observed + 1, db(active), db(inactive),
            valid && active > 0 && observed == expected,
            valid && std::abs(db(active) - level_db) <= tolerance_db,
            valid && active > 0 && (exact ? inactive == 0 : inactive <= active * amplitude(isolation_db))});
    }
    return result;
}
void check_software_edges(const SampleFormat& format, std::span<const BYTE> bytes, const OutputOrder& order,
                          std::optional<unsigned> single_output) {
    // Check the entire buffer, including every fade and guard interval, independently of the tone generator.
    const auto slots = single_output ? 1u : format.channels;
    for (std::size_t i = 0; i < bytes.size() / format.stride(); ++i) {
        const auto time = static_cast<double>(i) / format.rate - Leading;
        const auto slot = time < 0 ? slots : static_cast<unsigned>(time / Slot);
        const bool active = slot < slots && time - Slot * slot < ToneLength;
        for (unsigned channel = 0; channel < format.channels; ++channel) {
            if (active && channel == order[single_output.value_or(slot)]) continue;
            const BYTE* sample = bytes.data() + i * format.stride() + channel * format.bytes;
            require(std::all_of(sample, sample + format.bytes, [](BYTE value) { return value == 0; }),
                "Unexpected signal outside the selected software output");
        }
    }
}
bool pass_all(const Results& results) {
    return std::all_of(results.begin(), results.end(), [](const Result& row) {
        return row.order_pass && row.level_pass && row.isolation_pass;
    });
}
void write_results(std::ostream& out, const char* point, const ChannelMap& map, const Results& results) {
    const auto label = [](bool passed) { return passed ? "PASS" : "FAIL"; };
    for (const auto& row : results) {
        const auto& output = map.outputs[row.expected - 1];
        out << point << '\t' << map.outputs.size() << '\t' << row.logical << '\t'
            << row.expected << '\t' << row.observed << '\t' << output.connector << '\t'
            << output.speaker << '\t' << row.level << '\t' << row.inactive << '\t'
            << label(row.order_pass) << '\t' << label(row.level_pass) << '\t' << label(row.isolation_pass) << '\n';
    }
    require(static_cast<bool>(out), "Could not write the measurement report");
}
std::ofstream report(const fs::path& path) {
    std::ofstream out(path);
    out << "measurement_point\toutput_count\tlogical_channel\texpected_lane\tobserved_lane\tphysical_connector\tspeaker"
        "\tactive_peak_dBFS\tmax_other_peak_dBFS\torder\tlevel\tisolation\n";
    require(static_cast<bool>(out), "Could not open the measurement report");
    return out;
}
std::vector<BYTE> render_test(const SampleFormat& format, const ChannelMap& map, float level,
                              std::optional<unsigned> single_output = {}) {
    const ToneSequence tones(format, map, level, single_output);
    std::vector<BYTE> buffer(tones.frames() * format.stride());
    tones.write(buffer.data(), 0, static_cast<unsigned>(tones.frames()));
    check_software_edges(format, buffer, verified_order(map), single_output);
    return buffer;
}

// The first audible slot is logical channel one. Capture lanes use the same clock and stream order.
std::size_t sync(const SampleFormat& format, std::span<const BYTE> data, float level) {
    const auto frames = data.size() / format.stride();
    const float threshold = amplitude(level - 22);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (unsigned lane = 0; lane < format.channels; ++lane) {
            if (std::abs(format.decode(data.data() + frame * format.stride() + lane * format.bytes)) > threshold)
                return frame;
        }
    }
    throw std::runtime_error("No reference tone detected in the recording");
}

template<class T> Com<T> audio_service(IAudioClient& client) {
    T* raw{};
    checked(client.GetService(__uuidof(T), reinterpret_cast<void**>(&raw)), "Get a WASAPI stream service");
    return Com<T>(raw);
}
struct Running {
    IAudioClient& client;
    explicit Running(IAudioClient& c) : client(c) { checked(client.Start(), "Start WASAPI"); }
    ~Running() { client.Stop(); }
};

void drain_capture(IAudioCaptureClient& capture, const SampleFormat& format, std::vector<BYTE>& recording,
                   std::size_t limit, bool& discontinuity) {
    UINT32 ready{};
    while (SUCCEEDED(capture.GetNextPacketSize(&ready)) && ready) {
        BYTE* data{};
        DWORD flags{};
        UINT32 count{};
        checked(capture.GetBuffer(&data, &count, &flags, nullptr, nullptr), "Read WASAPI loopback");
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !recording.empty()) discontinuity = true;
        const auto bytes = static_cast<std::size_t>(count) * format.stride();
        if (recording.size() + bytes <= limit) {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) recording.resize(recording.size() + bytes, 0);
            else recording.insert(recording.end(), data, data + bytes);
        } else {
            discontinuity = true;
        }
        checked(capture.ReleaseBuffer(count), "Release WASAPI loopback");
    }
}

void play_exclusive(IMMDevice& device, Com<IAudioClient> client, const WAVEFORMATEX& wave,
                    const SampleFormat& format, std::span<const BYTE> stream) {
    check_format(*client, wave, AUDCLNT_SHAREMODE_EXCLUSIVE);
    REFERENCE_TIME minimum{};
    checked(client->GetDevicePeriod(nullptr, &minimum), "Get the endpoint's minimum period");
    REFERENCE_TIME period = std::max<REFERENCE_TIME>(200000, minimum);
    const auto initialize = [&] {
        return client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  period, period, &wave, nullptr);
    };
    HRESULT result = initialize();
    if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned{};
        checked(client->GetBufferSize(&aligned), "Read the aligned exclusive buffer size");
        period = (10'000'000LL * aligned + format.rate / 2) / format.rate;
        client.reset();
        client = audio_client(device);
        result = initialize();
    }
    checked(result, "Initialize WASAPI exclusive output");
    const std::unique_ptr<void, decltype(&CloseHandle)> ready(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
    require(static_cast<bool>(ready), "Could not create the exclusive render event");
    checked(client->SetEventHandle(ready.get()), "Register the exclusive render event");
    UINT32 capacity{};
    checked(client->GetBufferSize(&capacity), "Read the exclusive render buffer size");
    require(capacity > 0, "The exclusive endpoint returned an empty render buffer");
    const auto render = audio_service<IAudioRenderClient>(*client);
    const std::size_t packet_size = static_cast<std::size_t>(capacity) * format.stride();
    const auto fill_packet = [&](std::span<const BYTE> bytes) {
        BYTE* destination{};
        checked(render->GetBuffer(capacity, &destination), "Get the exclusive render packet");
        if (!bytes.empty()) std::memcpy(destination, bytes.data(), bytes.size());
        std::memset(destination + bytes.size(), 0, packet_size - bytes.size());
        checked(render->ReleaseBuffer(capacity, 0), "Submit the exclusive render packet");
    };
    const auto wait_packet = [&] {
        require(WaitForSingleObject(ready.get(), 3000) == WAIT_OBJECT_0,
            "The exclusive endpoint did not signal its render event");
    };
    auto sent = std::min(packet_size, stream.size());
    fill_packet(stream.first(sent));
    Running playback(*client);
    while (sent < stream.size()) {
        wait_packet();
        const auto size = std::min(packet_size, stream.size() - sent);
        fill_packet(stream.subspan(sent, size));
        sent += size;
    }
    wait_packet();
    fill_packet({});
    wait_packet();
}

Results play_shared(IMMDevice& device, IAudioClient& client, const WAVEFORMATEX& wave, const SampleFormat& format,
                    std::span<const BYTE> stream, const OutputOrder& order, std::optional<unsigned> single_output) {
    check_format(client, wave, AUDCLNT_SHAREMODE_SHARED);
    REFERENCE_TIME period{}, minimum{};
    checked(client.GetDevicePeriod(&period, &minimum), "Get the endpoint's processing period");
    const REFERENCE_TIME buffer_time = std::max<REFERENCE_TIME>(200000, minimum);
    checked(client.Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buffer_time, 0, &wave, nullptr), "Initialize WASAPI shared output");
    UINT32 capacity{};
    checked(client.GetBufferSize(&capacity), "Get the output buffer size");
    const auto render = audio_service<IAudioRenderClient>(client);

    const auto loop = audio_client(device);
    checked(loop->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, buffer_time, 0,
        &wave, nullptr), "Initialize WASAPI loopback");
    const auto capture = audio_service<IAudioCaptureClient>(*loop);
    Running reading(*loop);
    const auto render_bytes = [&](UINT32 frames, std::size_t byte_offset) {
        BYTE* dest{};
        checked(render->GetBuffer(frames, &dest), "Get output frames");
        // This verified, immutable PCM is copied directly into the endpoint buffer.
        std::memcpy(dest, stream.data() + byte_offset, frames * format.stride());
        checked(render->ReleaseBuffer(frames, 0), "Submit output frames");
    };
    const size_t total_frames = stream.size() / format.stride();
    auto submitted = std::min<std::size_t>(capacity, total_frames);
    render_bytes(static_cast<UINT32>(submitted), 0);
    std::vector<BYTE> recording;
    const auto capture_limit = stream.size() + static_cast<std::size_t>(format.rate) * format.stride() * 2;
    recording.reserve(capture_limit);
    bool discontinuity = false;
    Running playback(client);
    const auto wait_until = GetTickCount64() + total_frames * 1000 / format.rate + 2500;
    for (;;) {
        drain_capture(*capture, format, recording, capture_limit, discontinuity);
        UINT32 padding{};
        checked(client.GetCurrentPadding(&padding), "Read output buffer padding");
        if (submitted == total_frames && !padding) break;
        if (GetTickCount64() > wait_until) throw std::runtime_error("WASAPI did not drain the test stream");
        const auto count = static_cast<UINT32>(std::min<std::size_t>(capacity - padding, total_frames - submitted));
        if (count) {
            render_bytes(count, submitted * format.stride());
            submitted += count;
        }
        Sleep(3);
    }
    drain_capture(*capture, format, recording, capture_limit, discontinuity);
    require(!discontinuity, "Discontinuity in the WASAPI loopback, repeat with the endpoint idle");
    const auto first_frame = sync(format, recording, DeviceLevel);
    return measure(format, recording, order, first_frame,
        DeviceLevel, false, 1.f, -60.f, single_output);
}

int verify_recording(const fs::path& recording, std::wstring_view id, const fs::path& map_path,
                     const fs::path& report_path, float peak_reference, const wchar_t* logical_channel) {
    std::ifstream source(recording, std::ios::binary);
    const auto read = [&](void* dest, std::size_t bytes) {
        require(static_cast<bool>(source.read(static_cast<char*>(dest), static_cast<std::streamsize>(bytes))),
            "Truncated WAV capture");
    };
    std::uint32_t riff[3]{};
    read(riff, sizeof(riff));
    require(riff[0] == 0x46464952 && riff[2] == 0x45564157, "Capture must be a RIFF/WAVE file");
    const auto file_size = fs::file_size(recording);
    const auto riff_end = std::uint64_t{riff[1]} + 8;
    require(riff_end <= file_size && riff_end >= sizeof(riff), "Invalid WAV file size");
    WAVEFORMATEXTENSIBLE wave{};
    bool has_format = false;
    while (static_cast<std::uint64_t>(source.tellg()) + 8 <= riff_end) {
        std::uint32_t chunk[2]{};
        read(chunk, sizeof(chunk));
        const auto first = static_cast<std::uint64_t>(source.tellg());
        const auto after = first + chunk[1] + (chunk[1] & 1u);
        require(after <= riff_end, "Invalid WAV chunk size");
        if (chunk[0] == 0x20746d66) {
            require(!has_format && (chunk[1] == 16 || chunk[1] == 18 || chunk[1] == sizeof(wave)),
                "Unsupported WAV format description");
            read(&wave, chunk[1]);
            if (wave.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                require(chunk[1] == sizeof(wave), "Incomplete WAVEFORMATEXTENSIBLE metadata");
            has_format = true;
        }
        if (chunk[0] == 0x61746164) {
            require(has_format, "WAV format must precede its sample data");
            const SampleFormat format(wave.Format);
            require(chunk[1] > 0 && chunk[1] % format.stride() == 0 &&
                chunk[1] <= static_cast<std::uint64_t>(format.rate) * format.stride() * 60,
                "Expected a synchronized capture of at most one minute");
            const auto map = read_map(map_path, id, format.channels);
            std::optional<unsigned> single_output;
            if (logical_channel) single_output = number(logical_channel, 1, format.channels) - 1;
            std::vector<BYTE> data(chunk[1]);
            read(data.data(), data.size());
            const auto results = measure(format, data, verified_order(map), sync(format, data, peak_reference),
                peak_reference, false, 2.f, -60.f, single_output);
            auto out = report(report_path);
            write_results(out, "external_capture", map, results);
            require(pass_all(results), "External capture showed incorrect order, level or isolation");
            std::puts("External capture passed for all mapped lanes at the supplied reference level.");
            return 0;
        }
        source.seekg(static_cast<std::streamoff>(after));
    }
    throw std::runtime_error("No audio data found in the WAV capture");
}

void enumerate(IMMDeviceEnumerator& enumerator) {
    IMMDeviceCollection* raw{};
    checked(enumerator.EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &raw), "List active render endpoints");
    const Com<IMMDeviceCollection> collection(raw);
    UINT count{};
    checked(collection->GetCount(&count), "Count active render endpoints");
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* item{};
        checked(collection->Item(i, &item), "Get endpoint");
        const Com<IMMDevice> device(item);
        const auto id = endpoint_id(*device);
        IPropertyStore* raw_properties{};
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &raw_properties))) {
            const Com<IPropertyStore> properties(raw_properties);
            PROPVARIANT name{};
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
                std::wprintf(L"%ls\n", name.pwszVal);
            PropVariantClear(&name);
        }
        try {
            const auto client = audio_client(*device);
            const auto wave = shared_format(*client);
            const auto positions = mask(*wave);
            const auto matches = match_presets(wave->nChannels, positions);
            std::wprintf(L"  %ls  channels=%u rate=%lu mask=0x%08lx", id.c_str(),
                wave->nChannels, wave->nSamplesPerSec, static_cast<unsigned long>(positions));
            check_format(*client, *wave, AUDCLNT_SHAREMODE_SHARED);
            const auto labels = default_layout(id, wave->nChannels, positions);
            if (labels.outputs.front().speaker.empty()) std::wprintf(L"  assignment=manual");
            else if (matches.empty()) std::wprintf(L"  assignment=Windows-mask");
            for (const auto name : matches) std::wprintf(L"  preset=%hs", std::string(name).c_str());
            std::wprintf(L"\n");
        } catch (const std::runtime_error& error) {
            std::fprintf(stderr, "%s\n", error.what());
        }
    }
}

int self_test(const fs::path& output) {
    const auto wave = raw_format(24, 48000, 32, true);
    const SampleFormat format(wave.Format);
    auto map = default_layout(L"offline-24", format.channels, 0);
    constexpr std::array<unsigned, BedChannels> destinations{1, 0, 5, 23, 2, 7};
    for (unsigned source = 0; source < destinations.size(); ++source)
        map.outputs[destinations[source]].bed_channel = source;
    const auto matrix = make_bed_matrix(map);
    require(matrix.has_value(), "The six-channel bed could not be mapped to 24 outputs");
    for (unsigned out = 0; out < format.channels; ++out) {
        for (unsigned in = 0; in < BedChannels; ++in)
            require((*matrix)[out * BedChannels + in] == (out == destinations[in] ? 1.f : 0.f),
                "The gameplay matrix changed the channel order or level");
    }
    // Exercise the same LPCM packer as the game's WASAPI path with nonzero, signed bed samples.
    constexpr std::array<std::int16_t, BedChannels * 2> bed{
        16384, -12288, 8192, -4096, 2048, -1024, -16384, 12288, -8192, 4096, -2048, 1024
    };
    for (unsigned channels = 1; channels <= MaximumOutputs; ++channels) {
        auto direct_map = default_layout(L"direct-fixture", channels, 0);
        std::array<unsigned, BedChannels> targets{};
        const unsigned used = std::min(channels, BedChannels);
        for (unsigned i = 0; i < used; ++i) {
            targets[i] = channels - 1 - i;
            direct_map.outputs[targets[i]].bed_channel = i;
        }
        const auto route = make_bed_matrix(direct_map, channels >= BedChannels);
        require(route.has_value(), "The game PCM map rejected a valid count");
        for (const unsigned bits : {16u, 24u, 32u}) {
            const auto wave = raw_format(channels, 48000, bits);
            const SampleFormat pcm(wave.Format);
            std::vector<BYTE> packed(pcm.stride() * 2, 0xff);
            constexpr float gain = .5f;
            require(write_direct_pcm(*route, bed, packed, channels, bits, gain), "The game PCM packer rejected a valid format");
            for (unsigned frame = 0; frame < 2; ++frame) {
                for (unsigned lane = 0; lane < channels; ++lane) {
                    float expected = 0;
                    for (unsigned source = 0; source < used; ++source)
                        if (targets[source] == lane) expected = bed[frame * BedChannels + source] * gain / 32768.f;
                    require(pcm.decode(packed.data() + frame * pcm.stride() + lane * pcm.bytes) == expected,
                        "Game PCM order, gain or inactive channel changed");
                }
            }
        }
    }
    auto data = render_test(format, map, SoftwareLevel);
    const auto order = verified_order(map);
    const auto first = static_cast<std::size_t>(Leading * format.rate);
    const auto results = measure(format, data, order, first, SoftwareLevel, true, .01f, -100.f);
    require(pass_all(results), "Software channel isolation test failed");
    // Corrupt independently chosen samples to verify that leakage, order and level are detected.
    const auto fault_frame = first + format.rate / 8;
    auto* frame = data.data() + fault_frame * format.stride();
    format.encode(frame + format.bytes, .02f);
    require(!measure(format, data, order, first, SoftwareLevel, true, .01f, -100.f)[0].isolation_pass,
        "The diagnostic missed leakage");
    format.encode(frame + format.bytes, .25f);
    require(!measure(format, data, order, first, SoftwareLevel, true, .01f, -100.f)[0].order_pass,
        "The diagnostic missed an output order error");
    format.encode(frame + format.bytes, 0);
    format.encode(frame, .5f);
    require(!measure(format, data, order, first, SoftwareLevel, true, .01f, -100.f)[0].level_pass,
        "The diagnostic missed a level error");

    auto out = report(output);
    write_results(out, "software-legacy", map, results);
    for (unsigned channels = 1; channels <= MaximumOutputs; ++channels) {
        const auto format = raw_format(channels, 8000, 16);
        const SampleFormat pcm(format.Format);
        auto index_map = default_layout(L"offline", channels, 0);
        for (unsigned lane = 0; lane < channels; ++lane)
            index_map.outputs[lane].logical_channel = (lane + 1) % channels + 1;
        const auto stream = render_test(pcm, index_map, SoftwareLevel);
        OutputOrder expected{};
        for (unsigned logical = 0; logical < channels; ++logical) expected[logical] = (logical + channels - 1) % channels;
        const auto start = static_cast<std::size_t>(Leading * pcm.rate);
        const auto measurements = measure(pcm, stream, expected, start, SoftwareLevel, true, .02f, -100.f);
        require(pass_all(measurements), "PCM channel count test failed");
        write_results(out, "software", index_map, measurements);
        const auto single = render_test(pcm, index_map, SoftwareLevel, channels - 1);
        const auto single_result = measure(pcm, single, expected, start, SoftwareLevel, true, .02f, -100.f, channels - 1);
        require(single_result.size() == 1 && pass_all(single_result), "Single-channel PCM test failed");
        write_results(out, "software-single", index_map, single_result);
    }

    require(match_presets(8, 0x63f) == std::vector<std::string_view>{"7.1"} &&
        match_presets(8, 0x560f) == std::vector<std::string_view>{"5.1.2-front"} &&
        match_presets(6, 0x60f) == std::vector<std::string_view>{"5.1-side"} &&
        match_presets(6, 0x3f) == std::vector<std::string_view>{"5.1-rear"} &&
        match_presets(8, 0).empty(), "Speaker presets were matched by count or incorrect positions");
    const auto surround = preset_layout(L"offline", "7.1", 8, 0x63f);
    const auto surround_order = verified_order(surround);
    require(surround_order[4] == 6 && surround_order[6] == 4 && !supports_output_map(surround, 0x560f),
        "Incorrect Windows interleave for 7.1");
    const auto system_h = preset_layout(L"offline", "22.2", 24, 0);
    require(system_h.outputs[3].speaker == "LFE1" && system_h.outputs[9].speaker == "LFE2" &&
        make_preset_bed_matrix(system_h) == make_bed_matrix(system_h), "22.2 order or unmodified bed gain changed");
    for (const auto name : speaker_presets()) {
        const auto preset = preset_layout(L"offline", name, 0, 0);
        const auto matrix = make_preset_bed_matrix(preset);
        require(supports_output_map(preset, 0) && matrix.has_value(), "Invalid preset or missing gameplay matrix");
        for (unsigned lane = 0; lane < preset.outputs.size(); ++lane) {
            float sum = 0;
            for (unsigned source = 0; source < BedChannels; ++source) sum += (*matrix)[lane * BedChannels + source];
            require(sum <= 1.00001f, "Preset fold lacks headroom");
        }
    }
    const auto centre = stereo_speaker_gains(0);
    require(std::abs(centre[0] - centre[1]) < .00001f && centre[0] > .7f &&
        std::abs(centre[0] * centre[0] + centre[1] * centre[1] - 1.f) < .0001f,
        "Centre panning must distribute energy to both speakers");
    std::array<OutputChannel, MaximumOutputs> panning_layout{};
    panning_layout[2] = {"", "Left", -30.f, 0.f, {}};
    panning_layout[17] = {"", "Right", 30.f, 0.f, {}};
    panning_layout[9] = {"", "LFE2", 0.f, 0.f, {}};
    auto previous = directional_speaker_gains(panning_layout, {-.5f, 0, -.8660254f});
    require(previous[2] > previous[17], "Left panning used the wrong physical row");
    for (unsigned step = 1; step <= 120; ++step) {
        const float angle = (static_cast<float>(step) / 120 - .5f) * std::numbers::pi_v<float> / 3;
        const auto gains = directional_speaker_gains(panning_layout, {std::sin(angle), 0, -std::cos(angle)});
        require(gains[2] > 0 && gains[17] > 0 && gains[9] == 0 &&
            std::abs(gains[2] * gains[2] + gains[17] * gains[17] - 1.f) < .0001f &&
            std::abs(gains[2] - previous[2]) < .035f && std::abs(gains[17] - previous[17]) < .035f,
            "Directional panning leaked to LFE, changed total power or jumped between speakers");
        previous = gains;
    }
    require(previous[17] > previous[2], "Right panning used the wrong physical row");
    std::puts("Software isolation passed for 1-24 channels in sequence and single-output modes. Preset and fault checks passed.");
    return 0;
}

int command(int argc, wchar_t** argv) {
    const std::wstring_view mode = argc > 1 ? argv[1] : L"";
    if (mode == L"--self-test" && argc == 3) return self_test(argv[2]);
    if (mode == L"--verify-recording" && argc >= 6 && argc <= 8) {
        const float level = argc >= 7 ? -static_cast<float>(number(argv[6], 6, 90)) : DeviceLevel;
        return verify_recording(argv[4], argv[2], argv[3], argv[5], level, argc == 8 ? argv[7] : nullptr);
    }
    if (mode == L"--presets" && argc == 2) {
        std::puts("preset\toutput_count\tlogical_channel\tspeaker\tazimuth_deg\televation_deg");
        for (const auto name : speaker_presets()) {
            const auto preset = preset_layout(L"offline", name, 0, 0);
            for (const auto& output : preset.outputs) {
                std::printf("%s\t%u\t%u\t%s\t", preset.preset.c_str(), static_cast<unsigned>(preset.outputs.size()),
                    output.logical_channel, output.speaker.c_str());
                if (output.azimuth_degrees) std::printf("%g", static_cast<double>(*output.azimuth_degrees));
                std::printf("\t");
                if (output.elevation_degrees) std::printf("%g", static_cast<double>(*output.elevation_degrees));
                std::puts("");
            }
        }
        return 0;
    }
    const auto enumerator = devices();
    if (mode == L"--list" && argc == 2) {
        enumerate(*enumerator);
        return 0;
    }
    if (mode == L"--probe-exclusive" && argc == 6) {
        const auto selected = endpoint(*enumerator, argv[2]);
        const auto client = audio_client(*selected);
        const auto format = raw_format(number(argv[3], 1, MaximumOutputs), number(argv[4], 8000, MaxRate), number(argv[5], 16, 32));
        const SampleFormat validated(format.Format);
        check_format(*client, format.Format, AUDCLNT_SHAREMODE_EXCLUSIVE);
        std::printf("Driver accepts %u exclusive DirectOut channels at %u Hz.\n", validated.channels, validated.rate);
        return 0;
    }
    if (mode == L"--map" && (argc == 4 || argc == 5)) {
        const auto device = endpoint(*enumerator, argv[2]);
        const auto client = audio_client(*device);
        const auto wave = shared_format(*client);
        const auto channels = argc == 5 ? number(argv[4], 1, MaximumOutputs) : wave->nChannels;
        write_map(argv[3], default_layout(endpoint_id(*device), channels, argc == 5 ? 0 : mask(*wave)));
        return 0;
    }
    const bool raw_preset = mode == L"--preset-exclusive";
    if ((mode == L"--preset" && argc == 5) || (raw_preset && argc == 7)) {
        const auto device = endpoint(*enumerator, argv[2]);
        const auto client = audio_client(*device);
        const std::wstring_view requested(argv[3]);
        const std::string name(requested.begin(), requested.end());
        const auto id = endpoint_id(*device);
        if (raw_preset) {
            const auto map = preset_layout(id, name, 0, 0);
            const auto wave = raw_format(static_cast<unsigned>(map.outputs.size()),
                number(argv[5], 8000, MaxRate), number(argv[6], 16, 32));
            const SampleFormat format(wave.Format);
            check_format(*client, wave.Format, AUDCLNT_SHAREMODE_EXCLUSIVE);
            write_map(argv[4], map);
            std::puts("Exclusive format accepted. Confirm the connector order with a tone test.");
        } else {
            const auto wave = shared_format(*client);
            const SampleFormat format(*wave);
            check_format(*client, *wave, AUDCLNT_SHAREMODE_SHARED);
            const auto positions = mask(*wave);
            write_map(argv[4], preset_layout(id, name, format.channels, positions));
            if (!positions) std::puts("The driver has no speaker mask. Confirm the preset's connector order with a tone test.");
        }
        return 0;
    }
    const bool exclusive = mode == L"--play-exclusive";
    const bool raw_processing = mode == L"--play-raw";
    const int required_arguments = exclusive ? 8 : 5;
    if ((mode == L"--play-shared" || raw_processing || exclusive) &&
        (argc == required_arguments || argc == required_arguments + 1)) {
        const auto device = endpoint(*enumerator, argv[2]);
        auto client = audio_client(*device);
        if (raw_processing) request_raw_processing(*client);
        const auto shared = shared_format(*client);
        WAVEFORMATEXTENSIBLE raw{};
        if (exclusive)
            raw = raw_format(number(argv[5], 1, MaximumOutputs), number(argv[6], 8000, MaxRate), number(argv[7], 16, 32));
        const auto& wave = exclusive ? raw.Format : *shared;
        const SampleFormat format(wave);
        const auto map = read_map(argv[3], endpoint_id(*device), format.channels);
        require(supports_output_map(map, mask(wave)), "The speaker preset differs from the accepted format");
        std::optional<unsigned> single_output;
        if (argc == required_arguments + 1)
            single_output = number(argv[required_arguments], 1, format.channels) - 1;
        const auto order = verified_order(map);
        const auto stream = render_test(format, map, DeviceLevel, single_output);
        const auto software = measure(format, stream, order, static_cast<std::size_t>(Leading * format.rate),
            DeviceLevel, true, .03f, -100.f, single_output);
        require(pass_all(software), "Output buffer isolation test failed");
        auto out = report(argv[4]);
        write_results(out, "software", map, software);
        if (exclusive) {
            play_exclusive(*device, std::move(client), wave, format, stream);
        } else {
            const auto measured = play_shared(*device, *client, wave, format, stream, order, single_output);
            write_results(out, raw_processing ? "WASAPI_loopback_RAW_render" : "WASAPI_loopback", map, measured);
            require(pass_all(measured), "Loopback showed incorrect order, level or isolation. See the report");
        }
        std::puts(exclusive ? "Output buffer passed and the exclusive endpoint accepted the stream. Hardware capture pending."
                            : "Output buffer and WASAPI loopback passed. Physical connector isolation requires hardware capture.");
        return 0;
    }
    std::fwprintf(stderr,
        L"Usage:\n"
        L"  ac2_audio_outputs --list\n"
        L"  ac2_audio_outputs --presets\n"
        L"  ac2_audio_outputs --self-test <report.tsv>\n"
        L"  ac2_audio_outputs --map <endpoint-id|default> <map.tsv> [raw-channel-count]\n"
        L"  ac2_audio_outputs --preset <endpoint-id|default> <preset> <map.tsv>\n"
        L"  ac2_audio_outputs --preset-exclusive <endpoint-id|default> <preset> <map.tsv> <rate> <bits>\n"
        L"  ac2_audio_outputs --probe-exclusive <endpoint-id|default> <channels> <rate> <bits>\n"
        L"  ac2_audio_outputs --play-shared <endpoint-id|default> <map.tsv> <report.tsv> [logical-channel]\n"
        L"  ac2_audio_outputs --play-raw <endpoint-id|default> <map.tsv> <report.tsv> [logical-channel]\n"
        L"  ac2_audio_outputs --play-exclusive <endpoint-id> <map.tsv> <report.tsv> <channels> <rate> <bits> [logical-channel]\n"
        L"  ac2_audio_outputs --verify-recording <endpoint-id> <map.tsv> <capture.wav> <report.tsv> [level-below-full-scale [logical-channel]]\n");
    return 2;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init)) return 3;
    int result = 1;
    try {
        result = command(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
    }
    CoUninitialize();
    return result;
}
