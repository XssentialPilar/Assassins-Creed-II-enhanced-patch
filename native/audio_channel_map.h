#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ac2::audio {

constexpr unsigned MaximumOutputs = 24;
constexpr unsigned BedChannels = 6;
inline constexpr std::array<std::string_view, BedChannels> WwiseBed{"FL", "FR", "FC", "LFE", "BL", "BR"};

struct OutputChannel {
    std::string connector;
    std::string speaker;
    std::optional<float> azimuth_degrees;
    std::optional<float> elevation_degrees;
    std::optional<unsigned> bed_channel;
    unsigned logical_channel{}; // One-based channel in the program or preset.
};

struct ChannelMap {
    std::wstring endpoint_id;
    std::vector<OutputChannel> outputs; // Stream channels in driver order, beginning with channel 1.
    std::string preset{};
};

ChannelMap default_layout(std::wstring_view endpoint_id, unsigned channels, std::uint32_t speaker_mask);
std::span<const std::string_view> speaker_presets();
std::vector<std::string_view> match_presets(unsigned channels, std::uint32_t speaker_mask);
ChannelMap preset_layout(std::wstring_view endpoint_id, std::string_view preset, unsigned accepted_channels,
                         std::uint32_t speaker_mask);
bool supports_output_map(const ChannelMap& map, std::uint32_t speaker_mask);
void write_map(const std::filesystem::path& file, const ChannelMap& map);
ChannelMap read_map(const std::filesystem::path& file, std::wstring_view endpoint_id, unsigned channels);

using OutputOrder = std::array<unsigned, MaximumOutputs>;
std::optional<OutputOrder> output_order(const ChannelMap& map); // Logical order to zero-based stream lane.

// XAudio2 stores one row per destination. Unassigned outputs stay at zero.
using BedMatrix = std::array<float, MaximumOutputs * BedChannels>;
std::optional<BedMatrix> make_bed_matrix(const ChannelMap& map, bool require_complete_bed = true);
std::optional<BedMatrix> make_preset_bed_matrix(const ChannelMap& map);
bool write_direct_pcm(const BedMatrix& matrix, std::span<const std::int16_t> input,
                      std::span<std::uint8_t> output, unsigned channels, unsigned bits, float gain) noexcept;

std::array<float, 2> stereo_speaker_gains(float lateral) noexcept;
std::array<float, MaximumOutputs> directional_speaker_gains(std::span<const OutputChannel> layout,
    const std::array<float, 3>& arrival) noexcept;

}  // namespace ac2::audio
