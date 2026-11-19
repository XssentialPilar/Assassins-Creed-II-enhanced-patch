// The exclusive endpoint is explicitly selected. XAudio2 continues to own the shared path.
struct WasapiDirectSettings {
    std::wstring endpoint_id;
    unsigned channels{};
    unsigned bits{};
    ac2::audio::BedMatrix matrix{};
};

std::optional<WasapiDirectSettings> read_wasapi_direct_settings(const std::filesystem::path& folder) {
    const auto ini = folder / L"audio-direct.ini";
    if (GetPrivateProfileIntW(L"output", L"enabled", 0, ini.c_str()) != 1) return std::nullopt;
    const auto channels = GetPrivateProfileIntW(L"output", L"channels", 0, ini.c_str());
    const auto bits = GetPrivateProfileIntW(L"output", L"bits", 0, ini.c_str());
    wchar_t endpoint[512]{};
    const DWORD length = GetPrivateProfileStringW(L"output", L"endpoint", L"", endpoint, std::size(endpoint), ini.c_str());
    if (!length || length >= std::size(endpoint) - 1 || !channels || channels > ac2::audio::MaximumOutputs ||
        (bits != 16 && bits != 24 && bits != 32)) throw std::runtime_error("Invalid WASAPI direct settings");
    const auto map = ac2::audio::read_map(folder / L"audio-direct-map.tsv", endpoint, channels);
    const auto matrix = map.preset.empty()
        ? ac2::audio::make_bed_matrix(map, channels >= SourceChannels)
        : ac2::audio::make_preset_bed_matrix(map);
    if (!matrix) throw std::runtime_error("The direct map has no compatible Wwise routing");
    return WasapiDirectSettings{endpoint, channels, bits, *matrix};
}

class WwiseWasapiDirect {
    static constexpr unsigned QueueFrames = RingBytes / FrameBytes;
    using Event = std::unique_ptr<void, decltype(&CloseHandle)>;
    Event stop_{nullptr, CloseHandle}, ready_{nullptr, CloseHandle}, begin_{nullptr, CloseHandle};
    std::thread thread_;
    const WasapiDirectSettings settings_;
    std::array<std::int16_t, QueueFrames * SourceChannels> source_{};
    std::atomic<unsigned> written_{}, consumed_{}, played_{};
    std::atomic<float> gain_{1};
    std::atomic<HRESULT> result_{E_PENDING};

public:
    explicit WwiseWasapiDirect(WasapiDirectSettings settings) : settings_(std::move(settings)) {}
    ~WwiseWasapiDirect() {
        if (thread_.joinable()) {
            SetEvent(stop_.get());
            thread_.join();
        }
    }

    HRESULT open(LONG volume) noexcept {
        set_volume(volume);
        stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ready_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        begin_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_ || !ready_ || !begin_) return E_OUTOFMEMORY;
        try {
            thread_ = std::thread([this] {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const HRESULT status = SUCCEEDED(com) ? run() : com;
                if (SUCCEEDED(com)) CoUninitialize();
                if (FAILED(status)) {
                    result_.store(status, std::memory_order_release);
                    SetEvent(ready_.get());
                }
            });
        } catch (const std::exception&) { return E_FAIL; }
        if (WaitForSingleObject(ready_.get(), INFINITE) != WAIT_OBJECT_0) return E_FAIL;
        return result_.load(std::memory_order_acquire);
    }

    HRESULT set_volume(LONG millibels) noexcept {
        gain_.store(std::pow(10.f, static_cast<float>(millibels) / 2000.f), std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT submit(DWORD offset, const void* pcm, DWORD bytes) noexcept {
        const auto status = result_.load(std::memory_order_acquire);
        if (FAILED(status)) return status;
        const unsigned written = written_.load(std::memory_order_relaxed);
        const unsigned queued = written - consumed_.load(std::memory_order_acquire);
        if (!pcm || bytes != BlockBytes || offset != (written % QueueFrames) * FrameBytes) return E_INVALIDARG;
        if (queued > QueueFrames - BlockFrames) return E_PENDING;
        std::memcpy(source_.data() + (written % QueueFrames) * SourceChannels, pcm, bytes);
        written_.store(written + BlockFrames, std::memory_order_release);
        if (written == 2 * BlockFrames) SetEvent(begin_.get());
        return S_OK;
    }

    HRESULT cursor(DWORD& position, DWORD& queued) const noexcept {
        const auto played = played_.load(std::memory_order_acquire);
        const auto pending = written_.load(std::memory_order_acquire) - played;
        position = (played % QueueFrames) * FrameBytes;
        queued = (pending + BlockFrames - 1) / BlockFrames;
        return result_.load(std::memory_order_acquire);
    }

private:
    HRESULT run() noexcept {
        Event render_event(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
        if (!render_event) return HRESULT_FROM_WIN32(GetLastError());
        IMMDeviceEnumerator* raw_enumerator{};
        HRESULT status = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enumerator));
        if (FAILED(status)) return status;
        const AudioCom<IMMDeviceEnumerator> enumerator(raw_enumerator);
        IMMDevice* raw_device{};
        status = enumerator->GetDevice(settings_.endpoint_id.c_str(), &raw_device);
        if (FAILED(status)) return status;
        const AudioCom<IMMDevice> device(raw_device);
        IMMEndpoint* raw_direction{};
        status = device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&raw_direction));
        if (FAILED(status)) return status;
        const AudioCom<IMMEndpoint> direction(raw_direction);
        EDataFlow flow{};
        status = direction->GetDataFlow(&flow);
        if (FAILED(status) || flow != eRender) return FAILED(status) ? status : E_INVALIDARG;

        WAVEFORMATEXTENSIBLE wave{};
        const auto bytes = static_cast<WORD>(settings_.channels * (settings_.bits / 8));
        wave.Format = {WAVE_FORMAT_EXTENSIBLE, static_cast<WORD>(settings_.channels), SampleRate, SampleRate * bytes,
            bytes, static_cast<WORD>(settings_.bits), sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)};
        wave.Samples.wValidBitsPerSample = static_cast<WORD>(settings_.bits);
        wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        // DIRECTOUT has no named speaker mask. The map follows the driver's pin order.
        AudioCom<IAudioClient> client;
        const auto activate = [&]() {
            IAudioClient* raw_client{};
            const HRESULT status = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                reinterpret_cast<void**>(&raw_client));
            if (SUCCEEDED(status)) client.reset(raw_client);
            return status;
        };
        status = activate();
        if (FAILED(status)) return status;
        status = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wave.Format, nullptr);
        if (status != S_OK) return FAILED(status) ? status : AUDCLNT_E_UNSUPPORTED_FORMAT;
        REFERENCE_TIME period{};
        status = client->GetDevicePeriod(&period, nullptr);
        if (FAILED(status)) return status;
        const auto initialize = [&] {
            return client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                period, period, &wave.Format, nullptr);
        };
        status = initialize();
        if (status == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 frames{};
            status = client->GetBufferSize(&frames);
            if (FAILED(status)) return status;
            period = (10'000'000LL * frames + SampleRate / 2) / SampleRate;
            client.reset();
            status = activate();
            if (SUCCEEDED(status)) status = initialize();
        }
        if (FAILED(status)) return status;
        UINT32 packet{};
        status = client->GetBufferSize(&packet);
        if (FAILED(status)) return status;
        if (!packet || packet > BlockFrames) return AUDCLNT_E_BUFFER_SIZE_ERROR;
        status = client->SetEventHandle(render_event.get());
        if (FAILED(status)) return status;
        IAudioRenderClient* raw_render{};
        status = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&raw_render));
        if (FAILED(status)) return status;
        const AudioCom<IAudioRenderClient> render(raw_render);
        IAudioClock* raw_clock{};
        status = client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&raw_clock));
        if (FAILED(status)) return status;
        const AudioCom<IAudioClock> clock(raw_clock);
        UINT64 frequency{};
        status = clock->GetFrequency(&frequency);
        if (FAILED(status) || !frequency) return FAILED(status) ? status : E_FAIL;
        result_.store(S_OK, std::memory_order_release);
        SetEvent(ready_.get());
        trace("audio_wasapi_direct_device", 0, 0, settings_.channels, SampleRate, settings_.bits, packet, 0);

        const HANDLE startup[]{stop_.get(), begin_.get()};
        const DWORD start = WaitForMultipleObjects(std::size(startup), startup, FALSE, INFINITE);
        if (start != WAIT_OBJECT_0 + 1) return start == WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        status = render_packet(*render, packet);
        if (FAILED(status)) return status;
        status = client->Start();
        if (FAILED(status)) return status;
        const HANDLE events[]{stop_.get(), render_event.get()};
        UINT64 position{};
        while (true) {
            const DWORD event = WaitForMultipleObjects(std::size(events), events, FALSE, 1000);
            if (event == WAIT_OBJECT_0) break;
            if (event != WAIT_OBJECT_0 + 1) {
                status = event == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError());
                break;
            }
            status = clock->GetPosition(&position, nullptr);
            if (FAILED(status)) break;
            // WASAPI's per-stream playback clock starts at zero for a newly opened client.
            const auto frames = static_cast<unsigned>(
                position / frequency * SampleRate + position % frequency * SampleRate / frequency);
            const auto consumed = consumed_.load(std::memory_order_relaxed);
            if (consumed - frames <= QueueFrames) played_.store(frames, std::memory_order_release);
            status = render_packet(*render, packet);
            if (FAILED(status)) break;
        }
        client->Stop();
        return status;
    }

    HRESULT render_packet(IAudioRenderClient& render, unsigned packet) noexcept {
        const auto read = consumed_.load(std::memory_order_relaxed);
        if (written_.load(std::memory_order_acquire) - read < packet) return HRESULT_FROM_WIN32(ERROR_NO_DATA);
        BYTE* result{};
        const HRESULT status = render.GetBuffer(packet, &result);
        if (FAILED(status)) return status;
        const auto stride = settings_.channels * (settings_.bits / 8);
        const float gain = gain_.load(std::memory_order_relaxed);
        const auto start = read % QueueFrames;
        const auto first = std::min(packet, QueueFrames - start);
        const auto samples = std::span<const std::int16_t>(source_);
        const auto pcm = std::span(result, packet * stride);
        if (!ac2::audio::write_direct_pcm(settings_.matrix, samples.subspan(start * SourceChannels, first * SourceChannels),
                pcm.first(first * stride), settings_.channels, settings_.bits, gain) ||
            !ac2::audio::write_direct_pcm(settings_.matrix, samples.first((packet - first) * SourceChannels),
                pcm.subspan(first * stride), settings_.channels, settings_.bits, gain)) {
            render.ReleaseBuffer(packet, AUDCLNT_BUFFERFLAGS_SILENT);
            return E_INVALIDARG;
        }
        consumed_.store(read + packet, std::memory_order_release);
        return render.ReleaseBuffer(packet, 0);
    }
};
