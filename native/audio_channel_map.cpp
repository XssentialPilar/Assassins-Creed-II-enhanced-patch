#include "audio_channel_map.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <numbers>
#include <stdexcept>

namespace ac2::audio {
namespace {
constexpr std::string_view LegacyColumns = "channel\tconnector\tspeaker\tazimuth_deg\televation_deg\twwise_5_1";
constexpr std::string_view Columns = "channel\tconnector\tspeaker\tazimuth_deg\televation_deg\twwise_5_1\tlogical_channel";
constexpr std::string_view EndpointPrefix = "endpoint\t";
constexpr std::string_view PresetPrefix = "preset\t";
constexpr std::uint32_t NamedSpeakers = 0x3ffff;

struct NamedSpeaker {
    std::string_view name;
    float azimuth;
    float elevation;
};

// Nominal positions for the Windows labels. Angles are estimates, not measurements.
constexpr std::array<NamedSpeaker, 18> Speakers{{
    {"FL", -30, 0}, {"FR", 30, 0}, {"FC", 0, 0}, {"LFE", 0, 0},
    {"BL", -150, 0}, {"BR", 150, 0}, {"FLC", -15, 0}, {"FRC", 15, 0},
    {"BC", 180, 0}, {"SL", -90, 0}, {"SR", 90, 0}, {"TC", 0, 90},
    {"TFL", -30, 45}, {"TFC", 0, 45}, {"TFR", 30, 45},
    {"TBL", -150, 45}, {"TBC", 180, 45}, {"TBR", 150, 45}
}};

enum Position : unsigned { FL, FR, FC, LFE, BL, BR, FLC, FRC, BC, SL, SR, TC,
    TFL, TFC, TFR, TBL, TBC, TBR, TML, TMR, WL, WR };
constexpr NamedSpeaker Unmasked[]{{"TML", -90, 75}, {"TMR", 90, 75}, {"WL", -60, 0}, {"WR", 60, 0}};

// BS.2051-3 Table 10 order, with azimuth signs converted to the patch's right-positive convention.
constexpr NamedSpeaker ItuSystemH[]{
    {"FL",-60,0}, {"FR",60,0}, {"FC",0,0}, {"LFE1",-60,-25}, {"BL",-135,0}, {"BR",135,0},
    {"FLc",-30,0}, {"FRc",30,0}, {"BC",180,0}, {"LFE2",60,-25}, {"SiL",-90,0}, {"SiR",90,0},
    {"TpFL",-45,35}, {"TpFR",45,35}, {"TpFC",0,35}, {"TpC",0,90}, {"TpBL",-135,35}, {"TpBR",135,35},
    {"TpSiL",-90,35}, {"TpSiR",90,35}, {"TpBC",180,35}, {"BtFC",0,-25}, {"BtFL",-45,-25}, {"BtFR",45,-25}
};
static_assert(std::size(ItuSystemH) == MaximumOutputs);

struct Preset {
    std::string_view name;
    std::array<Position, MaximumOutputs> positions{};
    unsigned count{};
    std::uint32_t mask{};
};
constexpr Preset define_preset(std::string_view name, std::initializer_list<Position> positions) {
    Preset preset{name};
    bool mask_available = true;
    for (const auto position : positions) {
        preset.positions[preset.count++] = position;
        if (position < Speakers.size()) preset.mask |= 1u << position;
        else mask_available = false;
    }
    if (!mask_available) preset.mask = 0;
    return preset;
}
constexpr Preset Presets[]{
    define_preset("1.0", {FC}), define_preset("2.0", {FL,FR}), define_preset("2.1", {FL,FR,LFE}),
    define_preset("3.0", {FL,FR,FC}), define_preset("3.1", {FL,FR,FC,LFE}),
    define_preset("4.0-quad", {FL,FR,BL,BR}), define_preset("4.1", {FL,FR,LFE,BL,BR}),
    define_preset("5.0-side", {FL,FR,FC,SL,SR}), define_preset("5.0-rear", {FL,FR,FC,BL,BR}),
    define_preset("5.1-side", {FL,FR,FC,LFE,SL,SR}), define_preset("5.1-rear", {FL,FR,FC,LFE,BL,BR}),
    define_preset("6.1-side", {FL,FR,FC,LFE,SL,SR,BC}), define_preset("6.1-rear", {FL,FR,FC,LFE,BL,BR,BC}),
    define_preset("7.1", {FL,FR,FC,LFE,SL,SR,BL,BR}), define_preset("7.1-wide", {FL,FR,FC,LFE,FLC,FRC,BL,BR}),
    define_preset("5.1.2", {FL,FR,FC,LFE,SL,SR,TML,TMR}),
    define_preset("5.1.2-front", {FL,FR,FC,LFE,SL,SR,TFL,TFR}),
    define_preset("5.1.4", {FL,FR,FC,LFE,SL,SR,TFL,TFR,TBL,TBR}),
    define_preset("7.1.2", {FL,FR,FC,LFE,SL,SR,BL,BR,TML,TMR}),
    define_preset("7.1.2-front", {FL,FR,FC,LFE,SL,SR,BL,BR,TFL,TFR}),
    define_preset("7.1.4", {FL,FR,FC,LFE,SL,SR,BL,BR,TFL,TFR,TBL,TBR}),
    define_preset("9.1.4", {FL,FR,FC,LFE,WL,WR,SL,SR,BL,BR,TFL,TFR,TBL,TBR}),
    define_preset("9.1.6", {FL,FR,FC,LFE,WL,WR,SL,SR,BL,BR,TFL,TFR,TML,TMR,TBL,TBR}),
    {"22.2", {}, MaximumOutputs, 0}
};
constexpr auto PresetNames = [] {
    std::array<std::string_view, std::size(Presets)> names{};
    for (unsigned i = 0; i < names.size(); ++i) names[i] = Presets[i].name;
    return names;
}();

const Preset& get_preset(std::string_view name) {
    const auto found = std::find_if(std::begin(Presets), std::end(Presets),
        [name](const Preset& preset) { return preset.name == name; });
    if (found == std::end(Presets)) throw std::runtime_error("Unrecognized speaker preset");
    return *found;
}

std::optional<unsigned> bed_index(std::string_view name) {
    const auto item = std::find(WwiseBed.begin(), WwiseBed.end(), name);
    return item == WwiseBed.end() ? std::nullopt : std::optional<unsigned>(item - WwiseBed.begin());
}

std::string narrow_endpoint(std::wstring_view id) {
    if (id.empty() || std::any_of(id.begin(), id.end(), [](wchar_t c) { return c < 33 || c > 126; }))
        throw std::runtime_error("Invalid Windows endpoint ID");
    std::string text;
    text.reserve(id.size());
    for (const auto c : id) text.push_back(static_cast<char>(c));
    return text;
}

template<class Number>
Number parse_number(std::string_view value) {
    Number number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("Invalid number in output map");
    return number;
}

std::optional<float> parse_angle(std::string_view text, float limit) {
    if (text.empty()) return std::nullopt;
    const float angle = parse_number<float>(text);
    if (!std::isfinite(angle) || std::abs(angle) > limit) throw std::runtime_error("Speaker angle is outside its range");
    return angle;
}

std::array<std::string_view, 7> split_row(std::string_view row, bool legacy) {
    std::array<std::string_view, 7> parts{};
    const unsigned last = legacy ? 5 : 6;
    for (auto& part : std::span(parts).first(last)) {
        const auto delimiter = row.find('\t');
        if (delimiter == row.npos) throw std::runtime_error("Output map is missing a tab-separated column");
        part = row.substr(0, delimiter);
        row.remove_prefix(delimiter + 1);
    }
    if (row.find('\t') != row.npos) throw std::runtime_error("Too many columns in output map");
    parts[last] = row;
    return parts;
}

std::string get_line(std::istream& input) {
    std::string line;
    if (!std::getline(input, line) || line.size() > 4096) throw std::runtime_error("Incomplete or oversized output map");
    if (line.ends_with('\r')) line.pop_back();
    return line;
}
}  // namespace

ChannelMap default_layout(std::wstring_view endpoint_id, unsigned channels, std::uint32_t speaker_mask) {
    if (channels < 1 || channels > MaximumOutputs) throw std::runtime_error("Supported output count is 1–24");
    ChannelMap map{std::wstring(endpoint_id), std::vector<OutputChannel>(channels)};
    for (unsigned i = 0; i < channels; ++i) map.outputs[i].logical_channel = i + 1;
    if (speaker_mask == 0 || (speaker_mask & ~NamedSpeakers) != 0 ||
        static_cast<unsigned>(std::popcount(speaker_mask)) != channels) return map;
    unsigned index = 0;
    for (unsigned bit = 0; bit < Speakers.size(); ++bit) {
        if ((speaker_mask & (1u << bit)) == 0) continue;
        const auto& speaker = Speakers[bit];
        auto& output = map.outputs[index++];
        output.speaker = speaker.name;
        if (speaker.name != "LFE") {
            output.azimuth_degrees = speaker.azimuth;
            output.elevation_degrees = speaker.elevation;
        }
        output.bed_channel = bed_index(speaker.name);
    }
    return map;
}

std::span<const std::string_view> speaker_presets() { return PresetNames; }

std::vector<std::string_view> match_presets(unsigned channels, std::uint32_t speaker_mask) {
    std::vector<std::string_view> matches;
    if (speaker_mask) {
        for (const auto& preset : Presets)
            if (preset.count == channels && preset.mask == speaker_mask) matches.push_back(preset.name);
    }
    return matches;
}

ChannelMap preset_layout(std::wstring_view endpoint_id, std::string_view name, unsigned channels, std::uint32_t speaker_mask) {
    const auto& preset = get_preset(name);
    if (channels && channels != preset.count) throw std::runtime_error("Preset count differs from the endpoint format");
    if (speaker_mask && speaker_mask != preset.mask) throw std::runtime_error("Preset positions differ from the endpoint mask");
    auto map = default_layout(endpoint_id, preset.count, 0);
    map.preset = name;
    const bool itu = name == "22.2";
    const bool has_rear = std::find(preset.positions.begin(), preset.positions.begin() + preset.count, BL) !=
        preset.positions.begin() + preset.count;
    for (unsigned i = 0; i < preset.count; ++i) {
        const unsigned position = preset.positions[i];
        // Windows named output formats interleave channels in ascending mask bit order.
        const unsigned slot = speaker_mask ? std::popcount(speaker_mask & ((1u << position) - 1u)) : i;
        const auto& speaker = itu ? ItuSystemH[i] : position < Speakers.size() ? Speakers[position] : Unmasked[position - Speakers.size()];
        auto& output = map.outputs[slot];
        output.speaker = speaker.name;
        if (speaker.name != "LFE") {
            output.azimuth_degrees = speaker.azimuth;
            output.elevation_degrees = speaker.elevation;
        }
        if (!itu && (name == "4.0-quad" || name == "4.1") && (position == BL || position == BR))
            output.azimuth_degrees = position == BL ? -135.f : 135.f;
        output.logical_channel = i + 1;
        output.bed_channel = bed_index(speaker.name);
        if (speaker.name == "LFE1") output.bed_channel = 3;
        if (!has_rear && !itu && (position == SL || position == SR)) output.bed_channel = position == SL ? 4 : 5;
    }
    return map;
}

bool supports_output_map(const ChannelMap& map, std::uint32_t speaker_mask) {
    if (map.preset.empty()) return true;
    const auto& preset = get_preset(map.preset);
    if (preset.count != map.outputs.size() || (speaker_mask && speaker_mask != preset.mask)) return false;
    if (!speaker_mask) return true;
    const auto expected = preset_layout(map.endpoint_id, map.preset, preset.count, speaker_mask);
    for (unsigned slot = 0; slot < preset.count; ++slot) {
        if (map.outputs[slot].speaker != expected.outputs[slot].speaker ||
            map.outputs[slot].logical_channel != expected.outputs[slot].logical_channel) return false;
    }
    return true;
}

void write_map(const std::filesystem::path& file, const ChannelMap& map) {
    std::ofstream output(file);
    output << EndpointPrefix << narrow_endpoint(map.endpoint_id) << '\n';
    if (!map.preset.empty()) output << PresetPrefix << map.preset << '\n';
    output << Columns << '\n';
    for (unsigned i = 0; i < map.outputs.size(); ++i) {
        const auto& channel = map.outputs[i];
        output << i + 1 << '\t' << channel.connector << '\t' << channel.speaker << '\t';
        if (channel.azimuth_degrees) output << *channel.azimuth_degrees;
        output << '\t';
        if (channel.elevation_degrees) output << *channel.elevation_degrees;
        output << '\t';
        if (channel.bed_channel) output << WwiseBed[*channel.bed_channel];
        output << '\t' << channel.logical_channel << '\n';
    }
    if (!output) throw std::runtime_error("Could not write the output map");
}

ChannelMap read_map(const std::filesystem::path& file, std::wstring_view endpoint_id, unsigned channels) {
    if (channels < 1 || channels > MaximumOutputs) throw std::runtime_error("Supported output count is 1–24");
    std::ifstream input(file);
    if (!input) throw std::runtime_error("Output map was not found");
    std::string header = get_line(input);
    if (header.starts_with("\xef\xbb\xbf")) header.erase(0, 3);
    if (header != std::string(EndpointPrefix) + narrow_endpoint(endpoint_id))
        throw std::runtime_error("Output map belongs to a different Windows endpoint");
    ChannelMap map{std::wstring(endpoint_id), {}};
    auto columns = get_line(input);
    if (columns.starts_with(PresetPrefix)) {
        map.preset = columns.substr(PresetPrefix.size());
        if (get_preset(map.preset).count != channels) throw std::runtime_error("Map preset does not match the stream count");
        columns = get_line(input);
    }
    const bool legacy = columns == LegacyColumns;
    if (!legacy && columns != Columns) throw std::runtime_error("Unrecognized output map header");
    std::array<bool, BedChannels> assigned{};
    for (unsigned i = 0; i < channels; ++i) {
        const auto line = get_line(input);
        const auto column = split_row(line, legacy);
        if (parse_number<unsigned>(column[0]) != i + 1) throw std::runtime_error("Output channels must be in stream order");
        const auto bed = bed_index(column[5]);
        if (!column[5].empty() && (!bed || assigned[*bed])) throw std::runtime_error("Invalid or repeated Wwise role");
        if (bed) assigned[*bed] = true;
        map.outputs.push_back({std::string(column[1]), std::string(column[2]),
            parse_angle(column[3], 180), parse_angle(column[4], 90), bed,
            legacy ? i + 1 : parse_number<unsigned>(column[6])});
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Output map has extra channels");
    if (!output_order(map)) throw std::runtime_error("Logical channels must assign each stream lane exactly once");
    return map;
}

std::optional<OutputOrder> output_order(const ChannelMap& map) {
    if (map.outputs.empty() || map.outputs.size() > MaximumOutputs) return std::nullopt;
    OutputOrder order{};
    std::array<bool, MaximumOutputs> assigned{};
    for (unsigned slot = 0; slot < map.outputs.size(); ++slot) {
        const auto logical = map.outputs[slot].logical_channel;
        if (!logical || logical > map.outputs.size() || assigned[logical - 1]) return std::nullopt;
        assigned[logical - 1] = true;
        order[logical - 1] = slot;
    }
    return order;
}

std::optional<BedMatrix> make_bed_matrix(const ChannelMap& map, bool require_complete_bed) {
    if (map.outputs.empty() || map.outputs.size() > MaximumOutputs) return std::nullopt;
    BedMatrix matrix{};
    std::array<bool, BedChannels> assigned{};
    for (unsigned destination = 0; destination < map.outputs.size(); ++destination) {
        const auto source = map.outputs[destination].bed_channel;
        if (!source) continue;
        if (*source >= BedChannels || assigned[*source]) return std::nullopt;
        assigned[*source] = true;
        matrix[destination * BedChannels + *source] = 1;
    }
    if (require_complete_bed && std::find(assigned.begin(), assigned.end(), false) != assigned.end()) return std::nullopt;
    if (std::find(assigned.begin(), assigned.end(), true) == assigned.end()) return std::nullopt;
    return matrix;
}

std::optional<BedMatrix> make_preset_bed_matrix(const ChannelMap& map) {
    if (map.preset.empty()) return std::nullopt;
    auto matrix = make_bed_matrix(map, false);
    if (!matrix) return std::nullopt;
    std::array<unsigned, BedChannels> lanes{};
    lanes.fill(MaximumOutputs);
    for (unsigned slot = 0; slot < map.outputs.size(); ++slot)
        if (const auto bed = map.outputs[slot].bed_channel) lanes[*bed] = slot;

    constexpr float FoldGain = .70710678f;
    for (const unsigned source : {0u, 1u, 2u, 4u, 5u}) {
        if (lanes[source] != MaximumOutputs) continue;
        if (source == 2 && lanes[0] != MaximumOutputs && lanes[1] != MaximumOutputs) {
            (*matrix)[lanes[0] * BedChannels + source] = FoldGain;
            (*matrix)[lanes[1] * BedChannels + source] = FoldGain;
        } else {
            const unsigned front = (source == 1 || source == 5) ? 1 : 0;
            const auto target = lanes[front] != MaximumOutputs ? lanes[front] : lanes[2];
            if (target == MaximumOutputs) return std::nullopt;
            (*matrix)[target * BedChannels + source] = FoldGain;
        }
    }
    // Leave the original bed at unity when every speaker is present. A fold needs headroom.
    for (unsigned slot = 0; slot < map.outputs.size(); ++slot) {
        const auto row = std::span(*matrix).subspan(slot * BedChannels, BedChannels);
        float sum = 0;
        for (const auto gain : row) sum += gain;
        if (sum > 1) for (auto& gain : row) gain /= sum;
    }
    return matrix;
}

bool write_direct_pcm(const BedMatrix& matrix, std::span<const std::int16_t> input,
                      std::span<std::uint8_t> output, unsigned channels, unsigned bits, float gain) noexcept {
    if (!channels || channels > MaximumOutputs || (bits != 16 && bits != 24 && bits != 32) ||
        input.size() % BedChannels || !std::isfinite(gain) || gain < 0 || gain > 1) return false;
    const unsigned sample_bytes = bits / 8;
    const unsigned stride = channels * sample_bytes;
    const auto frames = input.size() / BedChannels;
    if (output.size() % stride || output.size() / stride != frames) return false;
    const auto limit = std::int64_t{1} << (bits - 1);
    const auto scale = gain * static_cast<double>(limit) / 32768.;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto samples = input.subspan(frame * BedChannels, BedChannels);
        for (unsigned lane = 0; lane < channels; ++lane) {
            const auto row = std::span(matrix).subspan(lane * BedChannels, BedChannels);
            double mix = 0;
            for (unsigned source = 0; source < BedChannels; ++source) mix += samples[source] * row[source];
            const auto sample = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                std::llround(mix * scale), -limit, limit - 1));
            std::memcpy(output.data() + frame * stride + lane * sample_bytes, &sample, sample_bytes);
        }
    }
    return true;
}

std::array<float, 2> stereo_speaker_gains(float lateral) noexcept {
    const float position = (std::clamp(lateral, -1.f, 1.f) + 1) * std::numbers::pi_v<float> / 4;
    if (lateral <= -1) return {1, 0};
    if (lateral >= 1) return {0, 1};
    return {std::cos(position), std::sin(position)};
}

std::array<float, MaximumOutputs> directional_speaker_gains(std::span<const OutputChannel> layout,
    const std::array<float, 3>& arrival) noexcept {
    constexpr float to_radians = std::numbers::pi_v<float> / 180;
    std::array<float, MaximumOutputs> gains{};
    float energy = 0;
    for (unsigned i = 0; i < std::min(gains.size(), layout.size()); ++i) {
        const auto& speaker = layout[i];
        if (!speaker.azimuth_degrees || !speaker.elevation_degrees || speaker.speaker.starts_with("LFE") ||
            speaker.bed_channel == 3u) continue;
        const float azimuth = *speaker.azimuth_degrees * to_radians;
        const float elevation = *speaker.elevation_degrees * to_radians;
        const float horizontal = std::cos(elevation);
        const float similarity = arrival[0] * horizontal * std::sin(azimuth) + arrival[1] * std::sin(elevation) -
            arrival[2] * horizontal * std::cos(azimuth);
        const float weight = std::exp(6.f * (std::clamp(similarity, -1.f, 1.f) - 1.f));
        gains[i] = weight;
        energy += weight * weight;
    }
    if (energy > 0) {
        const float normalization = 1 / std::sqrt(energy);
        for (float& gain : gains) gain *= normalization;
    }
    return gains;
}

}  // namespace ac2::audio
