#include "audio/AudioOut.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <mmreg.h>
#endif

namespace Sailor {

AudioOutManager::AudioOutManager() {
    InitWasapi();
}

AudioOutManager::~AudioOutManager() {
    ShutdownWasapi();
}

AudioOutManager& AudioOutManager::Instance() {
    static AudioOutManager instance;
    return instance;
}

void AudioOutManager::InitWasapi() {
#ifdef _WIN32
    if (m_wasapiInitialized) {
        return;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (FAILED(hr) || !enumerator) {
        SB_LOG_DEBUG("AudioOut", "WASAPI: No MMDeviceEnumerator available (hr=0x{:08X})", static_cast<u32>(hr));
        return;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
    if (FAILED(hr) || !device) {
        SB_LOG_DEBUG("AudioOut", "WASAPI: No default audio render endpoint found (hr=0x{:08X})", static_cast<u32>(hr));
        return;
    }

    Microsoft::WRL::ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr) || !client) {
        SB_LOG_DEBUG("AudioOut", "WASAPI: Failed to activate IAudioClient (hr=0x{:08X})", static_cast<u32>(hr));
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = client->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        SB_LOG_DEBUG("AudioOut", "WASAPI: Failed to get mix format (hr=0x{:08X})", static_cast<u32>(hr));
        return;
    }

    REFERENCE_TIME hnsBufferDuration = 1000000; // 100ms
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        SB_LOG_DEBUG("AudioOut", "WASAPI: Failed to initialize IAudioClient in shared mode (hr=0x{:08X})", static_cast<u32>(hr));
        CoTaskMemFree(mixFormat);
        return;
    }

    UINT32 bufferFrames = 0;
    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || bufferFrames == 0) {
        CoTaskMemFree(mixFormat);
        return;
    }

    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
    hr = client->GetService(__uuidof(IAudioRenderClient),
                            reinterpret_cast<void**>(renderClient.GetAddressOf()));
    if (FAILED(hr) || !renderClient) {
        CoTaskMemFree(mixFormat);
        return;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        return;
    }

    m_audioClient = client.Detach();
    m_renderClient = renderClient.Detach();
    m_waveFormat = mixFormat;
    m_bufferFrameCount = bufferFrames;
    m_wasapiInitialized = true;

    SB_LOG_INFO("AudioOut", "WASAPI: Initialized shared audio ring buffer (48kHz pipeline, channels={}, bufferFrames={})",
        mixFormat->nChannels, bufferFrames);
#endif
}

void AudioOutManager::ShutdownWasapi() {
#ifdef _WIN32
    if (m_audioClient) {
        IAudioClient* client = reinterpret_cast<IAudioClient*>(m_audioClient);
        client->Stop();
        client->Release();
        m_audioClient = nullptr;
    }
    if (m_renderClient) {
        IAudioRenderClient* render = reinterpret_cast<IAudioRenderClient*>(m_renderClient);
        render->Release();
        m_renderClient = nullptr;
    }
    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }
    m_wasapiInitialized = false;
#endif
}

void AudioOutManager::WriteToWasapi(const void* ptr, u32 sampleCount, u32 channels, u32 format, const float* channelVolumes, float masterGain) {
#ifdef _WIN32
    if (!m_wasapiInitialized || !m_audioClient || !m_renderClient || !m_waveFormat || !ptr || sampleCount == 0) {
        return;
    }

    IAudioClient* client = reinterpret_cast<IAudioClient*>(m_audioClient);
    IAudioRenderClient* render = reinterpret_cast<IAudioRenderClient*>(m_renderClient);
    WAVEFORMATEX* pwfx = reinterpret_cast<WAVEFORMATEX*>(m_waveFormat);

    UINT32 padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding))) {
        return;
    }

    UINT32 available = (m_bufferFrameCount > padding) ? (m_bufferFrameCount - padding) : 0;
    UINT32 toWrite = std::min(available, sampleCount);
    if (toWrite == 0) {
        return;
    }

    BYTE* pBuffer = nullptr;
    if (FAILED(render->GetBuffer(toWrite, &pBuffer)) || !pBuffer) {
        return;
    }

    const u32 outChannels = pwfx->nChannels;
    const bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    const float volL = (channelVolumes ? channelVolumes[0] : 1.0f) * masterGain;
    const float volR = (channelVolumes ? channelVolumes[1] : 1.0f) * masterGain;

    if (isFloat) {
        float* outFloat = reinterpret_cast<float*>(pBuffer);
        if (format == SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO) {
            const float* inFloat = reinterpret_cast<const float*>(ptr);
            for (UINT32 i = 0; i < toWrite; ++i) {
                float left = inFloat[i * channels + 0] * volL;
                float right = ((channels > 1) ? inFloat[i * channels + 1] : inFloat[i * channels + 0]) * volR;
                outFloat[i * outChannels + 0] = left;
                if (outChannels > 1) outFloat[i * outChannels + 1] = right;
                for (u32 c = 2; c < outChannels; ++c) {
                    float vc = (channelVolumes && c < 8) ? channelVolumes[c] * masterGain : masterGain;
                    outFloat[i * outChannels + c] = (c < channels) ? (inFloat[i * channels + c] * vc) : 0.0f;
                }
            }
        } else {
            const i16* inS16 = reinterpret_cast<const i16*>(ptr);
            for (UINT32 i = 0; i < toWrite; ++i) {
                float left = (static_cast<float>(inS16[i * channels + 0]) / 32768.0f) * volL;
                float right = (channels > 1) ? ((static_cast<float>(inS16[i * channels + 1]) / 32768.0f) * volR) : left;
                outFloat[i * outChannels + 0] = left;
                if (outChannels > 1) outFloat[i * outChannels + 1] = right;
                for (u32 c = 2; c < outChannels; ++c) {
                    float vc = (channelVolumes && c < 8) ? channelVolumes[c] * masterGain : masterGain;
                    outFloat[i * outChannels + c] = (c < channels) ? ((static_cast<float>(inS16[i * channels + c]) / 32768.0f) * vc) : 0.0f;
                }
            }
        }
    } else {
        i16* outS16 = reinterpret_cast<i16*>(pBuffer);
        if (format == SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO) {
            const float* inFloat = reinterpret_cast<const float*>(ptr);
            for (UINT32 i = 0; i < toWrite; ++i) {
                float left = std::clamp(inFloat[i * channels + 0] * volL, -1.0f, 1.0f);
                float right = (channels > 1) ? std::clamp(inFloat[i * channels + 1] * volR, -1.0f, 1.0f) : left;
                outS16[i * outChannels + 0] = static_cast<i16>(left * 32767.0f);
                if (outChannels > 1) outS16[i * outChannels + 1] = static_cast<i16>(right * 32767.0f);
                for (u32 c = 2; c < outChannels; ++c) outS16[i * outChannels + c] = 0;
            }
        } else {
            const i16* inS16 = reinterpret_cast<const i16*>(ptr);
            for (UINT32 i = 0; i < toWrite; ++i) {
                float left = std::clamp((static_cast<float>(inS16[i * channels + 0]) / 32768.0f) * volL, -1.0f, 1.0f);
                float right = (channels > 1) ? std::clamp((static_cast<float>(inS16[i * channels + 1]) / 32768.0f) * volR, -1.0f, 1.0f) : left;
                outS16[i * outChannels + 0] = static_cast<i16>(left * 32767.0f);
                if (outChannels > 1) outS16[i * outChannels + 1] = static_cast<i16>(right * 32767.0f);
                for (u32 c = 2; c < outChannels; ++c) outS16[i * outChannels + c] = 0;
            }
        }
    }

    render->ReleaseBuffer(toWrite, 0);
#endif
}

i32 AudioOutManager::Init() {
    std::lock_guard lock(m_mutex);
    m_initialized = true;
    InitWasapi();
    SB_LOG_INFO("AudioOut", "Initialized Multi-Channel Audio Engine (48kHz PlayStation Pipeline)");
    return SCE_OK;
}

i32 AudioOutManager::Open(i32 userId, i32 type, i32 index, u32 len, u32 freq, u32 param) {
    (void)index;
    std::lock_guard lock(m_mutex);
    if (!m_initialized) {
        m_initialized = true;
        SB_LOG_INFO("AudioOut", "Initialized Multi-Channel Audio Engine (48kHz PlayStation Pipeline)");
    }

    const i32 handle = m_nextHandle++;
    AudioPort port;
    port.handle = handle;
    port.userId = userId;
    port.type = type;
    port.sampleCount = (len > 0) ? len : 256;
    port.sampleRate = (freq > 0) ? freq : 48000;
    port.format = param;

    switch (param) {
        case SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO:
            port.channels = 1;
            break;
        case SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO:
        case SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO:
            port.channels = 2;
            break;
        case SCE_AUDIO_OUT_PARAM_FORMAT_S16_5DOT1:
            port.channels = 6;
            break;
        case SCE_AUDIO_OUT_PARAM_FORMAT_S16_7DOT1:
            port.channels = 8;
            break;
        default:
            port.channels = 2;
            break;
    }

    m_ports[handle] = port;
    SB_LOG_INFO("AudioOut", "Opened AudioOut handle={} (Type={}, Channels={}, Freq={}Hz, BufferLen={})",
        handle, type, port.channels, port.sampleRate, port.sampleCount);

    return handle;
}

i32 AudioOutManager::Close(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(handle);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    m_ports.erase(it);
    SB_LOG_DEBUG("AudioOut", "Closed AudioOut handle={}", handle);
    return SCE_OK;
}

i32 AudioOutManager::Output(i32 handle, const void* ptr) {
    if (!ptr || handle <= 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    u32 sampleCount = 0;
    u32 sampleRate = 48000;
    u32 channels = 2;
    u32 format = SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO;
    bool shouldPace = false;

    {
        std::lock_guard lock(m_mutex);
        auto it = m_ports.find(handle);
        if (it == m_ports.end()) {
            return SCE_KERNEL_ERROR_EINVAL;
        }

        AudioPort& port = it->second;
        const u32 sampleBytes = (port.format == SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO) ? 4 : 2;
        const size_t totalBytes = static_cast<size_t>(port.sampleCount) * port.channels * sampleBytes;

        port.lastBufferData.resize(totalBytes);
        std::memcpy(port.lastBufferData.data(), ptr, totalBytes);
        port.totalSamplesOutput += port.sampleCount;

        sampleCount = port.sampleCount;
        sampleRate = port.sampleRate ? port.sampleRate : 48000;
        channels = port.channels;
        format = port.format;
        shouldPace = m_pacingEnabled;

        float vols[8];
        std::memcpy(vols, port.channelVolumes, sizeof(vols));
        float gain = port.masterGain;

        // Direct write to WASAPI shared ring buffer with volume & gain attenuation
        WriteToWasapi(ptr, sampleCount, channels, format, vols, gain);
    }

    // Hardware pacing: pace the audio mixing thread to the hardware consumption rate
    if (shouldPace && sampleCount > 0 && sampleRate > 0) {
        const u64 sleepMicros = (static_cast<u64>(sampleCount) * 1'000'000ULL) / sampleRate;
        std::this_thread::sleep_for(std::chrono::microseconds(sleepMicros));
    }

    return SCE_OK;
}

i32 AudioOutManager::SetVolume(i32 handle, u32 flag, const i32* volumes) {
    (void)flag;
    if (!volumes || handle <= 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(handle);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    for (u32 c = 0; c < it->second.channels && c < 8; ++c) {
        // PlayStation volume range: 0..32768 (1.0 = 32768)
        it->second.channelVolumes[c] = std::clamp(static_cast<float>(volumes[c]) / 32768.0f, 0.0f, 2.0f);
    }
    return SCE_OK;
}

i32 AudioOutManager::GetPortInfo(i32 handle, SceAudioOutPortInfo* info) {
    if (!info || handle <= 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(handle);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    const AudioPort& port = it->second;
    info->channels = port.channels;
    info->sampleRate = port.sampleRate;
    info->format = port.format;
    info->sampleCount = port.sampleCount;
    info->samplesPlayed = port.totalSamplesOutput;
    return SCE_OK;
}

i32 AudioOutManager::SetPortGain(i32 handle, i32 gain) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(handle);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    it->second.masterGain = std::clamp(static_cast<float>(gain) / 1000.0f, 0.0f, 4.0f);
    return SCE_OK;
}

size_t AudioOutManager::GetOpenPortCount() {
    std::lock_guard lock(m_mutex);
    return m_ports.size();
}

bool AudioOutManager::GetLastOutputData(i32 handle, std::vector<u8>& outData) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(handle);
    if (it != m_ports.end()) {
        outData = it->second.lastBufferData;
        return true;
    }
    return false;
}

} // namespace Sailor
