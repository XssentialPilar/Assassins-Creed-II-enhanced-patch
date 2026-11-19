# Discrete audio outputs and channel verification

## Gameplay and output modes

The AC II bridge reads the original six-channel Wwise mix in `FL FR FC LFE BL BR` order. At startup it queries and checks the default endpoint's shared mix format with WASAPI. It then opens XAudio2 for that endpoint and checks the channel count and speaker mask reported by the mastering voice. DirectSound provides the existing compatibility fallback. The game trace records successful opens and the accepted layout as `audio_xaudio_device`, explicit matrix coefficients as `audio_xaudio_route`, and rejection details through `audio_xaudio_open_failed` and associated format or map events. [WASAPI format negotiation](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-isformatsupported) · [XAudio2 channel mapping](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-default-channel-mapping)

1. **Speaker presets.** Choose a named layout with a defined order and nominal speaker angles. On a masked stream, Windows interleaves the channels by ascending mask bit. The generated map translates preset order to that order. Exact count and mask distinguish layouts such as 7.1 and 5.1.2-front. A zero or unreliable mask requires a wiring assignment or an explicitly selected preset with its lane order checked. Small presets fold missing full-range Wwise channels with peak headroom. A complete six-role preset preserves the original Wwise bed at unity. Additional height and LFE lanes require independently authored signals or rendering before the Wwise mix.
2. **Direct output.** Assign logical channels to the driver's lanes for any count from 1 to 24. Each assigned game bed role is sent at unity and unused lanes receive zero. A manual map with fewer than six lanes sends the explicitly assigned subset of the bed. Use a named preset if that layout requires folding the remaining full-range bed channels. For six or more lanes, a direct gameplay map supplies all six bed roles. Existing six-column maps use their driver channel order as the logical order.

The preset catalog includes the following. Run `--presets` for the complete channel order and angles.

| Family | Presets |
| --- | --- |
| Basic | `1.0`, `2.0`, `2.1`, `3.0`, `3.1`, `4.0-quad`, `4.1`, `5.0-side`, `5.0-rear` |
| Surround | `5.1-side`, `5.1-rear`, `6.1-side`, `6.1-rear`, `7.1`, `7.1-wide` (Windows front-center pair) |
| Height | `5.1.2`, `5.1.2-front`, `5.1.4`, `7.1.2`, `7.1.2-front`, `7.1.4`, `9.1.4`, `9.1.6` |
| ITU | `22.2` (BS.2051-3 System H, Table 10) |

The height presets with top-middle pairs or front wides require a manual DirectOut map when Windows has no matching speaker bits. The ITU layout uses its specific 24-lane order, including LFE1 and LFE2 at logical channels 4 and 10. A bass-managed stereo endpoint exposes two channels when the receiver or speaker derives a subwoofer signal from stereo. [Dolby speaker layouts](https://www.dolby.com/about/support/guide/speaker-setup-guides/) · [ITU-R BS.2051-3](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2051-3-202205-I%21%21PDF-S.pdf)

### Map format

Put the map in `.ac2-enhanced-patch/audio-output-map.tsv` in the game directory. The first line binds it to one Windows endpoint ID. An optional `preset` line names a speaker preset. The header and channel rows use tabs. With no map, existing XAudio2 routing applies up to eight channels. Larger shared layouts use a complete Windows bed map or an exact known preset when possible. An unrecognized larger layout logs that a map is required and uses the compatibility fallback.

| Column | Meaning |
| --- | --- |
| `channel` | One-based driver stream channel. List every channel in order. |
| `connector` | The corresponding hardware or receiver output, confirmed by wiring or measurement. Leave unknown connectors blank. |
| `speaker` | Speaker connected to that output. A Windows mask supplies logical labels such as `FL`, `FR` or `TFL`. Use `LFE1`, `LFE2` and so on for distinctly wired bass outputs. |
| `azimuth_deg`, `elevation_deg` | Speaker angles relative to the listener. Front is 0°, right and up are positive. Windows labels receive nominal positions in a generated map. Replace these with measured angles when available, or clear them. |
| `wwise_5_1` | Original game role sent to that driver lane. Valid roles are `FL FR FC LFE BL BR`. Each role can be used once. |
| `logical_channel` | One-based logical channel for this lane. The full map must use every number from 1 to the stream count exactly once. The diagnostic uses this order. Earlier six-column files use `channel` as their logical order. |

Windows speaker masks describe named positions and a single standard LFE role. DirectOut with a zero mask exposes driver lanes in stream order. The map documents the connector once it is identified through wiring or measurement. [Windows channel masks](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/channel-mask) · [DirectOut format](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible)

### Exclusive DirectOut in the game

Use exclusive DirectOut when an interface exposes the required channel count only in exclusive mode. Generate or edit a map for the exact endpoint and put it in `.ac2-enhanced-patch/audio-direct-map.tsv`. In the same folder create `audio-direct.ini` with the endpoint ID reported by `--list`, the agreed channel count, and the PCM sample width.

```ini
[output]
enabled=1
endpoint=<exact Windows endpoint ID>
channels=24
bits=24
```

The bridge tests the exact 48 kHz format before use, then opens a WASAPI render thread and writes the mapped PCM to its event-driven buffers. Unsupported formats use the normal shared output and log the HRESULT as `audio_wasapi_direct_open_failed`. A successful exclusive open logs `audio_wasapi_direct_device`. If an active stream breaks or the source stops filling its ring in time, playback falls back to DirectSound and logs `audio_wasapi_direct_fallback`. Exclusive mode occupies the selected endpoint while the game audio stream is open. Disable it by setting `enabled=0` and restarting the game. [WASAPI exclusive streams](https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams)

## Build and test

The standalone diagnostic uses the same channel map code as the game. It builds with the Win32 LLVM-MinGW compiler already used by the patch.

```powershell
New-Item -ItemType Directory -Force -Path build/audio-outputs,dist/audio-outputs | Out-Null
i686-w64-mingw32-clang++ -std=c++20 -O2 -Wall -Wextra -Werror -municode -static -I native tools/audio_output_diagnostic.cpp native/audio_channel_map.cpp -lole32 -luuid -lpropsys -o build/audio-outputs/ac2_audio_outputs.exe
$tool = './build/audio-outputs/ac2_audio_outputs.exe'
& $tool --self-test ./dist/audio-outputs/software-1-24.tsv
& $tool --list
& $tool --presets
& $tool --map default ./dist/audio-outputs/output-map.tsv
& $tool --play-shared default ./dist/audio-outputs/output-map.tsv ./dist/audio-outputs/shared-test.tsv
& $tool --preset default 2.0 ./dist/audio-outputs/stereo-map.tsv
& $tool --play-raw default ./dist/audio-outputs/stereo-map.tsv ./dist/audio-outputs/left-test.tsv 1
```

`--map` also accepts an endpoint ID from `--list` instead of `default`. Use the endpoint ID when verifying more than one device. Add a count after the map filename to generate an unnamed DirectOut template for an exact count such as 24. The connector column starts blank until the physical wiring is known.

The software pass measures the level, order and exact zero samples on inactive outputs before submission. It checks counts 1–24, a reordered logical map, all lanes in sequence, a single selected lane for each count, and deliberate leakage, order and gain errors. Each row reports the logical, expected driver and observed driver channels. The selected lane has unity routing gain. The generated tone is −36 dBFS for device tests, with fades and quiet intervals. The synthetic software-only run uses −24 dBFS. Add a final logical channel argument to any playback command to test one output only.

`--play-shared` also captures the Windows endpoint loopback, which can include effects and other applications. It checks order, ±1 dB of the requested peak, and at least 60 dB of separation between active and inactive channels. `--play-raw` requests RAW processing for the render stream. Device-wide effects can still apply. Neither operation changes Windows configuration. [WASAPI loopback](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording) · [RAW and endpoint effects](https://learn.microsoft.com/windows-hardware/drivers/audio/audio-processing-object-architecture)

```powershell
& $tool --probe-exclusive $endpointId 24 48000 24
& $tool --map $endpointId ./dist/audio-outputs/output-map.tsv 24
& $tool --preset-exclusive $endpointId 22.2 ./dist/audio-outputs/itu-map.tsv 48000 24
& $tool --play-exclusive $endpointId ./dist/audio-outputs/output-map.tsv ./dist/audio-outputs/exclusive-test.tsv 24 48000 24
& $tool --verify-recording $endpointId ./dist/audio-outputs/output-map.tsv ./capture.wav ./dist/audio-outputs/hardware-test.tsv
```

Exclusive mode confirms format negotiation and submission through event-driven WASAPI buffers. For physical validation, connect each mapped output to independent capture inputs or a measurement system. Record one synchronized WAV with lanes in the intended stream order. Start before the first tone and record through the last. Check and record the physical connector and speaker in the map. Calibrate the capture path so the reference peak is known. `--verify-recording` defaults to −36 dBFS and accepts a positive number for the magnitude of another reference level below full scale, followed by the logical channel for a single-output capture if applicable. It requires correct order, ±2 dB of the calibrated peak and inactive signal at least 60 dB below the active signal. [WASAPI exclusive streams](https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams)

The release routes the existing six-channel Wwise mix. In-game acoustic portals and per-source binaural stereo remain in development. Physical output verification depends on the interface and its wiring.
