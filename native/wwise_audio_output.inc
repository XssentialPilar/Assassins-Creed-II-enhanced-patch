// Included by d3d9_probe.cpp so the bridge uses the executable and trace helpers.
constexpr DWORD WwiseDirectSoundSite = 0x0135f4b2;
constexpr DWORD WwiseOutputBufferSite = 0x0135ed3d;
constexpr DWORD BlockFrames = 1024;
constexpr DWORD SampleRate = 48000;
constexpr DWORD SourceChannels = 6;
constexpr DWORD FrameBytes = SourceChannels * sizeof(std::int16_t);
constexpr DWORD BlockBytes = BlockFrames * FrameBytes;
constexpr DWORD RingBytes = BlockBytes * 4;
constexpr DWORD SourceMask = 0x3f;  // FL, FR, FC, LFE, BL, BR
constexpr DWORD WriteLeadBytes = 1440 * FrameBytes;
constexpr UINT32 NoVirtualAudioClient = 0x10000;
static_assert(SourceChannels == ac2::audio::BedChannels);

std::filesystem::path wwise_output_folder() {
    wchar_t executable[MAX_PATH];
    const DWORD size = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!size || size >= MAX_PATH) throw std::runtime_error("The game path is unavailable");
    return std::filesystem::path(executable).parent_path() / L".ac2-enhanced-patch";
}

std::optional<ac2::audio::BedMatrix> wwise_output_matrix(std::wstring_view endpoint_id, unsigned channels,
                                                       DWORD mask) noexcept {
    try {
        const auto file = wwise_output_folder() / L"audio-output-map.tsv";
        if (!std::filesystem::exists(file)) {
            if (channels <= 8) return std::nullopt;
            const auto legacy = ac2::audio::make_bed_matrix(ac2::audio::default_layout(endpoint_id, channels, mask));
            if (legacy) return legacy;
            const auto matches = ac2::audio::match_presets(channels, mask);
            if (matches.size() == 1)
                return ac2::audio::make_preset_bed_matrix(
                    ac2::audio::preset_layout(endpoint_id, matches.front(), channels, mask));
            return std::nullopt;
        }
        const auto map = ac2::audio::read_map(file, endpoint_id, channels);
        if (!ac2::audio::supports_output_map(map, mask)) {
            trace("audio_xaudio_preset_mask_mismatch", 0, 0, channels, 0, 0, 0, mask);
            return std::nullopt;
        }
        const auto matrix = map.preset.empty()
            ? ac2::audio::make_bed_matrix(map, channels >= SourceChannels)
            : ac2::audio::make_preset_bed_matrix(map);
        if (!matrix) trace("audio_xaudio_map_bed_incomplete", 0, 0, channels);
        return matrix;
    } catch (const std::exception&) {
        trace("audio_xaudio_map_parse_failed", 0, 0, channels, 0, 0, 0, mask);
        return std::nullopt;
    }
}

template <class T>
struct AudioRelease {
    void operator()(T* value) const noexcept { value->Release(); }
};

template <class T>
using AudioCom = std::unique_ptr<T, AudioRelease<T>>;

#include "wwise_wasapi_direct.inc"

class WwiseOutput final : public IXAudio2EngineCallback, public IXAudio2VoiceCallback {
    struct Block {
        std::array<BYTE, BlockBytes> pcm{};
        std::atomic<bool> in_use{};
    };

    HMODULE module_{};
    AudioCom<IXAudio2> engine_;
    IXAudio2MasteringVoice* master_{};
    IXAudio2SourceVoice* source_{};
    bool registered_{};
    bool started_{};
    std::atomic<HRESULT> error_{S_OK};
    std::array<Block, 4> blocks_;
    unsigned submitted_{};
    DWORD position_{};
    std::unique_ptr<WwiseWasapiDirect> direct_;

public:
    ~WwiseOutput() { reset(); }
    bool uses_direct() const noexcept { return static_cast<bool>(direct_); }

    HRESULT open(const WAVEFORMATEXTENSIBLE& format, LONG volume) noexcept {
        try {
            if (auto settings = read_wasapi_direct_settings(wwise_output_folder())) {
                auto direct = std::make_unique<WwiseWasapiDirect>(std::move(*settings));
                const HRESULT result = direct->open(volume);
                if (SUCCEEDED(result)) {
                    direct_ = std::move(direct);
                    return S_OK;
                }
                trace("audio_wasapi_direct_open_failed", 0, 0, 0, 0, 0, 0, result);
            }
        } catch (const std::exception&) {
            trace("audio_wasapi_direct_settings_invalid", 0, 0);
        }
        // The renderer can run on Wwise's audio thread, which may not have COM initialized.
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return apartment;
        const auto uninitialize = [](void*) { CoUninitialize(); };
        const std::unique_ptr<void, decltype(uninitialize)> com_guard(
            SUCCEEDED(apartment) ? this : nullptr, uninitialize);

        IMMDeviceEnumerator* raw_enumerator{};
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enumerator));
        if (FAILED(result)) return result;
        const AudioCom<IMMDeviceEnumerator> enumerator(raw_enumerator);
        IMMDevice* raw_endpoint{};
        result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &raw_endpoint);
        if (FAILED(result)) return result;
        const AudioCom<IMMDevice> endpoint(raw_endpoint);
        IAudioClient* raw_client{};
        result = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(&raw_client));
        if (FAILED(result)) return result;
        const AudioCom<IAudioClient> client(raw_client);
        WAVEFORMATEX* raw_format{};
        result = client->GetMixFormat(&raw_format);
        if (FAILED(result)) return result;
        const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix(raw_format, CoTaskMemFree);
        if (!mix || mix->nChannels == 0 || mix->nChannels > ac2::audio::MaximumOutputs ||
            mix->nChannels > XAUDIO2_MAX_AUDIO_CHANNELS) return E_INVALIDARG;
        WAVEFORMATEX* closest_format{};
        result = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, mix.get(), &closest_format);
        CoTaskMemFree(closest_format);
        if (result != S_OK) {
            trace("audio_wasapi_shared_format_rejected", 0, 0, mix->nChannels, mix->nSamplesPerSec, 0, 0, result);
            return FAILED(result) ? result : AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        LPWSTR raw_endpoint_id{};
        result = endpoint->GetId(&raw_endpoint_id);
        if (FAILED(result)) return result;
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> endpoint_id(raw_endpoint_id, CoTaskMemFree);
        const DWORD endpoint_mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix.get())->dwChannelMask
            : mix->nChannels == 1 ? 4 : mix->nChannels == 2 ? 3 : 0;
        const auto matrix = wwise_output_matrix(endpoint_id.get(), mix->nChannels, endpoint_mask);
        if (mix->nChannels > 8 && !matrix) {
            trace("audio_xaudio_map_required", 0, 0, mix->nChannels, 0, 0, 0, endpoint_mask);
            return E_INVALIDARG;
        }

        module_ = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return HRESULT_FROM_WIN32(GetLastError());
        using Create = HRESULT (WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        const auto create = reinterpret_cast<Create>(GetProcAddress(module_, "XAudio2Create"));
        if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        IXAudio2* raw_engine{};
        result = create(&raw_engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(result)) return result;
        engine_.reset(raw_engine);
        result = engine_->RegisterForCallbacks(this);
        if (FAILED(result)) return result;
        registered_ = true;
        result = engine_->CreateMasteringVoice(&master_, mix->nChannels, XAUDIO2_DEFAULT_SAMPLERATE,
            NoVirtualAudioClient, endpoint_id.get(), nullptr, AudioCategory_GameEffects);
        if (FAILED(result)) {
            trace("audio_xaudio_master_rejected", 0, 0, mix->nChannels, mix->nSamplesPerSec, endpoint_mask, 0, result);
            return result;
        }
        XAUDIO2_VOICE_DETAILS details{};
        DWORD mask{};
        master_->GetVoiceDetails(&details);
        result = master_->GetChannelMask(&mask);
        if (FAILED(result)) return result;
        if (details.InputChannels != mix->nChannels || (endpoint_mask && endpoint_mask != mask)) {
            trace("audio_xaudio_master_layout_mismatch", 0, 0, details.InputChannels, mix->nChannels, mask, endpoint_mask);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        result = engine_->CreateSourceVoice(&source_, &format.Format, XAUDIO2_VOICE_NOPITCH, 1.f, this);
        if (FAILED(result)) return result;
        if (matrix) {
            result = source_->SetOutputMatrix(master_, SourceChannels, details.InputChannels, matrix->data());
            if (FAILED(result)) return result;
            for (unsigned destination = 0; destination < details.InputChannels; ++destination) {
                const auto start = matrix->begin() + destination * SourceChannels;
                for (unsigned source = 0; source < SourceChannels; ++source) {
                    if (start[source] != 0)
                        trace("audio_xaudio_route", 0, 0, destination + 1, source + 1, 0, 0, start[source]);
                }
            }
        }
        result = set_volume(volume);
        if (SUCCEEDED(result)) trace("audio_xaudio_device", 0, 0, details.InputChannels,
            details.InputSampleRate, SourceChannels, SampleRate, mask, SourceMask);
        return result;
    }

    HRESULT set_volume(LONG volume) const noexcept {
        if (direct_) return direct_->set_volume(volume);
        return source_->SetVolume(std::pow(10.f, static_cast<float>(volume) / 2000.f));
    }

    HRESULT submit(DWORD offset, const void* pcm, DWORD bytes) noexcept {
        if (direct_) return direct_->submit(offset, pcm, bytes);
        if (!pcm || bytes != BlockBytes || offset != (submitted_ % 4) * BlockBytes) return E_INVALIDARG;
        auto& block = blocks_[offset / BlockBytes];
        if (block.in_use.exchange(true, std::memory_order_acq_rel)) return E_PENDING;
        std::memcpy(block.pcm.data(), pcm, BlockBytes);
        XAUDIO2_BUFFER buffer{};
        buffer.pAudioData = block.pcm.data();
        buffer.AudioBytes = BlockBytes;
        buffer.pContext = &block;
        const HRESULT result = source_->SubmitSourceBuffer(&buffer);
        if (FAILED(result)) {
            block.in_use.store(false, std::memory_order_release);
            return result;
        }
        ++submitted_;
        if (submitted_ == 3) {
            const HRESULT status = source_->Start(0);
            if (FAILED(status)) return status;
            started_ = true;
        }
        if (submitted_ <= 4 || submitted_ % 256 == 0) {
            std::array<int, SourceChannels> peaks{};
            for (DWORD i = 0; i < BlockFrames; ++i) {
                for (DWORD channel = 0; channel < SourceChannels; ++channel) {
                    std::int16_t sample{};
                    std::memcpy(&sample, block.pcm.data() + i * FrameBytes + channel * sizeof(sample), sizeof(sample));
                    peaks[channel] = std::max(peaks[channel], std::abs(static_cast<int>(sample)));
                }
            }
            trace("audio_xaudio_pcm", submitted_, 0, peaks[0], peaks[1], peaks[2], peaks[3], peaks[4], peaks[5]);
        }
        return S_OK;
    }

    HRESULT cursor(DWORD& position, DWORD& queued) noexcept {
        if (direct_) return direct_->cursor(position, queued);
        const HRESULT result = error_.load(std::memory_order_acquire);
        if (FAILED(result)) {
            position = position_;
            return result;
        }
        XAUDIO2_VOICE_STATE state{};
        source_->GetState(&state, 0);
        position = position_ = static_cast<DWORD>(state.SamplesPlayed % (RingBytes / FrameBytes)) * FrameBytes;
        queued = state.BuffersQueued;
        if (started_ && !queued) return HRESULT_FROM_WIN32(ERROR_NO_DATA);
        return S_OK;
    }

    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) noexcept override { error_.store(error); }
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override { error_.store(error); }
    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override {
        static_cast<Block*>(context)->in_use.store(false, std::memory_order_release);
    }
    void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}

private:
    void reset() noexcept {
        if (source_) source_->DestroyVoice();
        if (master_) master_->DestroyVoice();
        if (registered_) engine_->UnregisterForCallbacks(this);
        engine_.reset();
        if (module_) FreeLibrary(module_);
    }
};

class WwiseSoundBuffer final : public IDirectSoundBuffer8 {
    AudioCom<IDirectSoundBuffer8> real_;
    const WAVEFORMATEXTENSIBLE format_;
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    std::unique_ptr<WwiseOutput> output_;
    DWORD play_offset_{};
    LONG volume_{};
    unsigned cursors_{};
    bool tried_{};

    HRESULT fallback(HRESULT error, DWORD position) noexcept {
        trace(output_->uses_direct() ? "audio_wasapi_direct_fallback" : "audio_xaudio_fallback",
            0, 0, position, 0, 0, 0, error);
        output_.reset();
        const HRESULT result = real_->SetCurrentPosition(position);
        return FAILED(result) ? result : real_->Play(0, 0, DSBPLAY_LOOPING);
    }

public:
    WwiseSoundBuffer(IDirectSoundBuffer8* real, const WAVEFORMATEXTENSIBLE& format) noexcept
        : real_(real), format_(format) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) noexcept override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDirectSoundBuffer || iid == IID_IDirectSoundBuffer8) {
            *object = static_cast<IDirectSoundBuffer8*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(iid, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG count = --refs_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDSBCAPS caps) noexcept override { return real_->GetCaps(caps); }
    HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX format, DWORD bytes, LPDWORD written) noexcept override {
        return real_->GetFormat(format, bytes, written);
    }
    HRESULT STDMETHODCALLTYPE GetVolume(LPLONG volume) noexcept override { return real_->GetVolume(volume); }
    HRESULT STDMETHODCALLTYPE GetPan(LPLONG pan) noexcept override { return real_->GetPan(pan); }
    HRESULT STDMETHODCALLTYPE GetFrequency(LPDWORD rate) noexcept override { return real_->GetFrequency(rate); }
    HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD status) noexcept override {
        const std::lock_guard lock(mutex_);
        if (output_ && status) {
            *status = DSBSTATUS_PLAYING | DSBSTATUS_LOOPING;
            return S_OK;
        }
        return real_->GetStatus(status);
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTSOUND sound, LPCDSBUFFERDESC description) noexcept override {
        return real_->Initialize(sound, description);
    }

    HRESULT STDMETHODCALLTYPE Play(DWORD reserved, DWORD priority, DWORD flags) noexcept override {
        const std::lock_guard lock(mutex_);
        if (output_ && flags == DSBPLAY_LOOPING && !reserved && !priority) return S_OK;
        if (output_) output_.reset();
        if (!tried_ && !reserved && !priority && !play_offset_ && flags == DSBPLAY_LOOPING) {
            tried_ = true;
            output_.reset(new (std::nothrow) WwiseOutput);
            const HRESULT result = output_ ? output_->open(format_, volume_) : E_OUTOFMEMORY;
            if (SUCCEEDED(result)) return S_OK;
            trace("audio_xaudio_open_failed", 0, 0, 0, 0, 0, 0, result);
            output_.reset();
        }
        return real_->Play(reserved, priority, flags);
    }

    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD play, LPDWORD write) noexcept override {
        const std::lock_guard lock(mutex_);
        if (output_) {
            DWORD position{}, queued{};
            const HRESULT result = output_->cursor(position, queued);
            if (SUCCEEDED(result)) {
                if (play) *play = position;
                if (write) *write = (position + (queued ? WriteLeadBytes : 0)) % RingBytes;
                if (queued && ++cursors_ % 512 == 0) trace("audio_xaudio_cursor", 0, 0, position, queued);
                return S_OK;
            }
            const HRESULT switched = fallback(result, position);
            if (FAILED(switched)) return switched;
        }
        return real_->GetCurrentPosition(play, write);
    }

    HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, LPVOID* first, LPDWORD first_bytes,
                                   LPVOID* second, LPDWORD second_bytes, DWORD flags) noexcept override {
        const std::lock_guard lock(mutex_);
        if (output_ && (flags || bytes != BlockBytes || offset >= RingBytes || offset % BlockBytes)) {
            DWORD position{}, queued{};
            output_->cursor(position, queued);
            const HRESULT switched = fallback(E_INVALIDARG, position);
            if (FAILED(switched)) return switched;
        }
        const HRESULT result = real_->Lock(offset, bytes, first, first_bytes, second, second_bytes, flags);
        if (SUCCEEDED(result)) last_lock_offset_ = offset;
        return result;
    }

    HRESULT STDMETHODCALLTYPE Unlock(LPVOID first, DWORD first_bytes, LPVOID second, DWORD second_bytes) noexcept override {
        const std::lock_guard lock(mutex_);
        HRESULT submitted = S_OK;
        if (output_) {
            submitted = second_bytes ? E_INVALIDARG : output_->submit(last_lock_offset_, first, first_bytes);
        }
        const HRESULT result = real_->Unlock(first, first_bytes, second, second_bytes);
        if (FAILED(submitted)) {
            DWORD position{}, queued{};
            output_->cursor(position, queued);
            const HRESULT switched = fallback(submitted, position);
            if (FAILED(switched)) return switched;
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD position) noexcept override {
        const std::lock_guard lock(mutex_);
        if (output_) {
            const HRESULT switched = fallback(E_NOTIMPL, position);
            if (FAILED(switched)) return switched;
        }
        const HRESULT result = real_->SetCurrentPosition(position);
        if (SUCCEEDED(result)) play_offset_ = position;
        return result;
    }
    HRESULT STDMETHODCALLTYPE SetFormat(LPCWAVEFORMATEX format) noexcept override {
        return real_->SetFormat(format);
    }
    HRESULT STDMETHODCALLTYPE SetVolume(LONG volume) noexcept override {
        const std::lock_guard lock(mutex_);
        const HRESULT result = real_->SetVolume(volume);
        if (FAILED(result)) return result;
        volume_ = volume;
        return output_ ? output_->set_volume(volume) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPan(LONG pan) noexcept override { return real_->SetPan(pan); }
    HRESULT STDMETHODCALLTYPE SetFrequency(DWORD rate) noexcept override { return real_->SetFrequency(rate); }
    HRESULT STDMETHODCALLTYPE Stop() noexcept override {
        const std::lock_guard lock(mutex_);
        output_.reset();
        return real_->Stop();
    }
    HRESULT STDMETHODCALLTYPE Restore() noexcept override { return real_->Restore(); }
    HRESULT STDMETHODCALLTYPE SetFX(DWORD count, LPDSEFFECTDESC description, LPDWORD results) noexcept override {
        return real_->SetFX(count, description, results);
    }
    HRESULT STDMETHODCALLTYPE AcquireResources(DWORD flags, DWORD count, LPDWORD result) noexcept override {
        return real_->AcquireResources(flags, count, result);
    }
    HRESULT STDMETHODCALLTYPE GetObjectInPath(REFGUID classid, DWORD index, REFGUID iid, LPVOID* object) noexcept override {
        return real_->GetObjectInPath(classid, index, iid, object);
    }

private:
    DWORD last_lock_offset_{};
};

bool is_wwise_output(const DSBUFFERDESC* desc) noexcept {
    if (!desc || desc->dwBufferBytes != RingBytes || desc->dwFlags !=
        (DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2)) return false;
    const auto* wave = desc->lpwfxFormat;
    if (!wave || wave->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        wave->cbSize != sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) ||
        wave->nChannels != SourceChannels || wave->nSamplesPerSec != SampleRate ||
        wave->nBlockAlign != FrameBytes || wave->wBitsPerSample != 16) return false;
    const auto& format = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
    return format.dwChannelMask == SourceMask && format.Samples.wValidBitsPerSample == 16 &&
        format.SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
}

class WwiseDirectSound final : public IDirectSound8 {
    AudioCom<IDirectSound8> real_;
    std::atomic<ULONG> refs_{1};
public:
    explicit WwiseDirectSound(IDirectSound8* real) noexcept : real_(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) noexcept override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDirectSound || iid == IID_IDirectSound8) {
            *object = static_cast<IDirectSound8*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(iid, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG count = --refs_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* result, IUnknown* outer)
        noexcept override {
        const HRESULT status = real_->CreateSoundBuffer(desc, result, outer);
        if (SUCCEEDED(status) && result && *result && !outer &&
            code_rva(__builtin_return_address(0)) == WwiseOutputBufferSite && is_wwise_output(desc)) {
            IDirectSoundBuffer8* buffer{};
            if (SUCCEEDED((*result)->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&buffer)))) {
                const auto& format = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(desc->lpwfxFormat);
                if (auto* wrapper = new (std::nothrow) WwiseSoundBuffer(buffer, format)) {
                    (*result)->Release();
                    // The DirectSound headers declare Buffer8 as a separate COM interface with the Buffer prefix.
                    *result = reinterpret_cast<IDirectSoundBuffer*>(static_cast<IDirectSoundBuffer8*>(wrapper));
                    trace("audio_xaudio_bridge", 0, 0, SourceChannels, SampleRate, RingBytes, BlockBytes);
                } else buffer->Release();
            }
        }
        return status;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDSCAPS caps) noexcept override { return real_->GetCaps(caps); }
    HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER buffer, LPDIRECTSOUNDBUFFER* copy)
        noexcept override { return real_->DuplicateSoundBuffer(buffer, copy); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD level) noexcept override {
        return real_->SetCooperativeLevel(window, level);
    }
    HRESULT STDMETHODCALLTYPE Compact() noexcept override { return real_->Compact(); }
    HRESULT STDMETHODCALLTYPE GetSpeakerConfig(LPDWORD config) noexcept override { return real_->GetSpeakerConfig(config); }
    HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD config) noexcept override { return real_->SetSpeakerConfig(config); }
    HRESULT STDMETHODCALLTYPE Initialize(LPCGUID device) noexcept override { return real_->Initialize(device); }
    HRESULT STDMETHODCALLTYPE VerifyCertification(LPDWORD verified) noexcept override {
        return real_->VerifyCertification(verified);
    }
};

using CreateComObject = decltype(&CoCreateInstance);
CreateComObject wwise_create_com{};

HRESULT WINAPI create_wwise_audio(REFCLSID clsid, IUnknown* owner, DWORD context, REFIID iid, void** object) {
    const HRESULT status = wwise_create_com(clsid, owner, context, iid, object);
    if (SUCCEEDED(status) && object && *object && !owner &&
        clsid == CLSID_DirectSound8 && iid == IID_IDirectSound8 &&
        code_rva(__builtin_return_address(0)) == WwiseDirectSoundSite) {
        if (auto* wrapper = new (std::nothrow) WwiseDirectSound(static_cast<IDirectSound8*>(*object))) {
            *object = static_cast<IDirectSound8*>(wrapper);
        }
    }
    return status;
}

void install_wwise_audio_bridge() noexcept {
    const GameImage image = game_image();
    if (!image.base || image.image_size != 35192832 || image.code_size != 25837568) return;
    const auto* base = reinterpret_cast<const BYTE*>(image.base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x5925ba62 || nt->OptionalHeader.CheckSum != 0x211f312) return;
    const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.VirtualAddress > image.image_size ||
        imports.Size > image.image_size - imports.VirtualAddress) return;
    const auto* entries = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (DWORD i = 0; i < imports.Size / sizeof(*entries) && entries[i].Name; ++i) {
        const auto& entry = entries[i];
        if (entry.Name > image.image_size - sizeof("ole32.dll") || !entry.OriginalFirstThunk ||
            !entry.FirstThunk || _strnicmp(reinterpret_cast<const char*>(base + entry.Name),
                                           "ole32.dll", sizeof("ole32.dll")) != 0) continue;
        const DWORD table = std::max(entry.OriginalFirstThunk, entry.FirstThunk);
        if (table >= image.image_size) return;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + entry.OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(image.base + entry.FirstThunk);
        for (DWORD j = 0; j < (image.image_size - table) / sizeof(*names) && names[j].u1.AddressOfData; ++j) {
            const DWORD name = names[j].u1.AddressOfData;
            if (name > image.image_size - sizeof(WORD) - sizeof("CoCreateInstance")) continue;
            const auto* method = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name);
            if (std::memcmp(method->Name, "CoCreateInstance", sizeof("CoCreateInstance")) != 0) continue;
            DWORD previous{};
            if (!VirtualProtect(&slots[j].u1.Function, sizeof(DWORD), PAGE_READWRITE, &previous)) return;
            wwise_create_com = reinterpret_cast<CreateComObject>(slots[j].u1.Function);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&slots[j].u1.Function),
                reinterpret_cast<LONG>(&create_wwise_audio));
            DWORD ignored{};
            VirtualProtect(&slots[j].u1.Function, sizeof(DWORD), previous, &ignored);
            return;
        }
    }
}
