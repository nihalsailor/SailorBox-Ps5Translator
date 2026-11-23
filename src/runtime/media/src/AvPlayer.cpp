#include "media/AvPlayer.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>
#endif

namespace Sailor {

AvPlayerManager::AvPlayerManager() {
    InitMediaFoundation();
}

AvPlayerManager::~AvPlayerManager() {
    ShutdownMediaFoundation();
}

AvPlayerManager& AvPlayerManager::Instance() {
    static AvPlayerManager instance;
    return instance;
}

void AvPlayerManager::InitMediaFoundation() {
#ifdef _WIN32
    if (!m_mfStarted) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (SUCCEEDED(hr)) {
            m_mfStarted = true;
            SB_LOG_INFO("AvPlayer", "Initialized Windows Media Foundation Hardware Video Decoder");
        }
    }
#endif
}

void AvPlayerManager::ShutdownMediaFoundation() {
#ifdef _WIN32
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
#endif
}

bool AvPlayerManager::TryOpenSourceReader(AvPlayerStream& player, const std::string& uri) {
#ifdef _WIN32
    InitMediaFoundation();
    if (!m_mfStarted) {
        return false;
    }

    std::filesystem::path localPath(uri);
    if (!std::filesystem::exists(localPath)) {
        if (uri.starts_with("/app0/") || uri.starts_with("app0/")) {
            std::size_t skip = uri.starts_with('/') ? 6 : 5;
            localPath = std::filesystem::current_path() / uri.substr(skip);
        }
    }

    if (!std::filesystem::exists(localPath)) {
        return false;
    }

    std::wstring widePath = localPath.wstring();
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(widePath.c_str(), nullptr, reader.GetAddressOf());
    if (FAILED(hr) || !reader) {
        return false;
    }

    // Configure Video Stream (RGB32 for direct frame presentation)
    Microsoft::WRL::ComPtr<IMFMediaType> videoType;
    if (SUCCEEDED(MFCreateMediaType(videoType.GetAddressOf()))) {
        videoType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, videoType.Get()))) {
            Microsoft::WRL::ComPtr<IMFMediaType> currentType;
            if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, currentType.GetAddressOf()))) {
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
                    player.width = w;
                    player.height = h;
                }
            }
        }
    }

    // Configure Audio Stream (48kHz stereo PCM)
    Microsoft::WRL::ComPtr<IMFMediaType> audioType;
    if (SUCCEEDED(MFCreateMediaType(audioType.GetAddressOf()))) {
        audioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        audioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        audioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audioType.Get());
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) {
        if (var.vt == VT_UI8) {
            player.durationUs = var.uhVal.QuadPart / 10;
        }
        PropVariantClear(&var);
    }

    player.sourceReader = reader.Detach();
    player.hasRealDemuxer = true;

    // Pre-allocate frame and audio buffers to prevent null pData on stream ticks/loops
    const size_t initialVideoBytes = static_cast<size_t>(player.width) * player.height * 4;
    player.currentFrameRgba.resize(initialVideoBytes, 0);
    player.currentAudioPcm.resize(1024 * 2 * sizeof(i16), 0);

    SB_LOG_INFO("AvPlayer", "Demuxer attached for '{}' (Resolution={}x{}, Duration={:.2f}s)",
        uri, player.width, player.height, player.durationUs / 1000000.0f);
    return true;
#else
    (void)player;
    (void)uri;
    return false;
#endif
}

void AvPlayerManager::SynthesizeFrame(AvPlayerStream& player) {
    const size_t byteSize = static_cast<size_t>(player.width) * player.height * 4;
    if (player.currentFrameRgba.size() != byteSize) {
        player.currentFrameRgba.resize(byteSize);
    }

    // Dynamic video frame pattern based on frame index (color wipe pattern)
    const u8 r = static_cast<u8>((player.frameIndex * 3) % 256);
    const u8 g = static_cast<u8>((player.frameIndex * 5) % 256);
    const u8 b = static_cast<u8>((player.frameIndex * 7) % 256);
    const u8 a = 255;

    u32* pixels = reinterpret_cast<u32*>(player.currentFrameRgba.data());
    const u32 color = (a << 24) | (b << 16) | (g << 8) | r;
    const size_t numPixels = player.width * player.height;
    std::fill_n(pixels, numPixels, color);

    player.frameIndex++;
}

i32 AvPlayerManager::Init(const SceAvPlayerInitData* initData) {
    (void)initData;
    std::lock_guard lock(m_mutex);
    m_initialized = true;

    const i32 handle = m_nextHandle++;
    AvPlayerStream player;
    player.handle = handle;
    player.state = AvPlayerState::Idle;
    player.width = 1920;
    player.height = 1080;
    player.fps = 60.0f;
    player.durationUs = 10000000; // 10 seconds

    m_players[handle] = std::move(player);
    SB_LOG_INFO("AvPlayer", "Initialized AvPlayer instance handle={}", handle);
    return handle;
}

i32 AvPlayerManager::AddSource(i32 handle, const char* uri) {
    if (!uri || handle <= 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    it->second.sourceUri = uri;
    it->second.currentTimeUs = 0;
    it->second.frameIndex = 0;

    if (!TryOpenSourceReader(it->second, uri)) {
        SynthesizeFrame(it->second);
    }

    SB_LOG_INFO("AvPlayer", "Added video source '{}' to player handle={}", uri, handle);
    return SCE_OK;
}

i32 AvPlayerManager::Start(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    it->second.state = AvPlayerState::Playing;
    SB_LOG_INFO("AvPlayer", "Started video playback on handle={}", handle);
    return SCE_OK;
}

i32 AvPlayerManager::Stop(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    it->second.state = AvPlayerState::Stopped;
    it->second.currentTimeUs = 0;
    SB_LOG_INFO("AvPlayer", "Stopped video playback on handle={}", handle);
    return SCE_OK;
}

i32 AvPlayerManager::Pause(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    it->second.state = AvPlayerState::Paused;
    return SCE_OK;
}

bool AvPlayerManager::IsActive(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end()) {
        return false;
    }
    return it->second.state == AvPlayerState::Playing || it->second.state == AvPlayerState::Paused;
}

bool AvPlayerManager::GetVideoData(i32 handle, SceAvPlayerVideoData* videoData) {
    if (!videoData || handle <= 0) {
        return false;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end() || it->second.state != AvPlayerState::Playing) {
        return false;
    }

    AvPlayerStream& player = it->second;

    if (player.hasRealDemuxer && player.sourceReader) {
#ifdef _WIN32
        IMFSourceReader* reader = reinterpret_cast<IMFSourceReader*>(player.sourceReader);
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp100ns = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;

        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp100ns, sample.GetAddressOf());
        if (SUCCEEDED(hr)) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (player.loop) {
                    PROPVARIANT var;
                    PropVariantInit(&var);
                    var.vt = VT_I8;
                    var.hVal.QuadPart = 0;
                    reader->SetCurrentPosition(GUID_NULL, var);
                    PropVariantClear(&var);
                    player.currentTimeUs = 0;
                } else {
                    player.state = AvPlayerState::Stopped;
                    return false;
                }
            }

            if (sample) {
                Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(mediaBuffer.GetAddressOf()))) {
                    BYTE* pBuffer = nullptr;
                    DWORD currentLength = 0;
                    if (SUCCEEDED(mediaBuffer->Lock(&pBuffer, nullptr, &currentLength))) {
                        player.currentFrameRgba.resize(currentLength);
                        std::memcpy(player.currentFrameRgba.data(), pBuffer, currentLength);
                        mediaBuffer->Unlock();
                        player.currentTimeUs = static_cast<u64>(timestamp100ns / 10);
                    }
                }
            }
        }
#endif
    } else {
        SynthesizeFrame(player);
        player.currentTimeUs += 16666;
        if (player.currentTimeUs >= player.durationUs) {
            if (player.loop) {
                player.currentTimeUs = 0;
            } else {
                player.state = AvPlayerState::Stopped;
            }
        }
    }

    const size_t minVideoBytes = static_cast<size_t>(player.width) * player.height * 4;
    if (player.currentFrameRgba.size() < minVideoBytes) {
        player.currentFrameRgba.resize(minVideoBytes, 0);
    }

    videoData->timestamp = player.currentTimeUs;
    videoData->width = player.width;
    videoData->height = player.height;
    videoData->pitch = player.width * 4;
    videoData->aspectWidth = 16;
    videoData->aspectHeight = 9;
    videoData->pData = player.currentFrameRgba.data();

    return true;
}

bool AvPlayerManager::GetAudioData(i32 handle, SceAvPlayerAudioData* audioData) {
    if (!audioData || handle <= 0) {
        return false;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end() || it->second.state != AvPlayerState::Playing) {
        return false;
    }

    AvPlayerStream& player = it->second;

    if (player.hasRealDemuxer && player.sourceReader) {
#ifdef _WIN32
        IMFSourceReader* reader = reinterpret_cast<IMFSourceReader*>(player.sourceReader);
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp100ns = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;

        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &timestamp100ns, sample.GetAddressOf());
        if (SUCCEEDED(hr) && sample) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(mediaBuffer.GetAddressOf()))) {
                BYTE* pBuffer = nullptr;
                DWORD currentLength = 0;
                if (SUCCEEDED(mediaBuffer->Lock(&pBuffer, nullptr, &currentLength))) {
                    player.currentAudioPcm.resize(currentLength);
                    std::memcpy(player.currentAudioPcm.data(), pBuffer, currentLength);
                    mediaBuffer->Unlock();
                }
            }
        }
#endif
    } else {
        const size_t audioBytes = 1024 * 2 * sizeof(i16); // 1024 stereo samples
        if (player.currentAudioPcm.size() != audioBytes) {
            player.currentAudioPcm.resize(audioBytes, 0);
        }
    }

    const size_t minAudioBytes = 1024 * 2 * sizeof(i16);
    if (player.currentAudioPcm.size() < minAudioBytes) {
        player.currentAudioPcm.resize(minAudioBytes, 0);
    }

    audioData->timestamp = player.currentTimeUs;
    audioData->channelNum = 2;
    audioData->sampleRate = 48000;
    audioData->size = static_cast<u32>(player.currentAudioPcm.size());
    audioData->pData = player.currentAudioPcm.data();

    return true;
}

i32 AvPlayerManager::SetLooping(i32 handle, bool loop) {
    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    it->second.loop = loop;
    return SCE_OK;
}

i32 AvPlayerManager::Close(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it == m_players.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    if (it->second.sourceReader) {
#ifdef _WIN32
        reinterpret_cast<IUnknown*>(it->second.sourceReader)->Release();
        it->second.sourceReader = nullptr;
#endif
    }

    m_players.erase(it);
    SB_LOG_DEBUG("AvPlayer", "Closed AvPlayer instance handle={}", handle);
    return SCE_OK;
}

size_t AvPlayerManager::GetActivePlayersCount() {
    std::lock_guard lock(m_mutex);
    return m_players.size();
}

AvPlayerState AvPlayerManager::GetPlayerState(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_players.find(handle);
    if (it != m_players.end()) {
        return it->second.state;
    }
    return AvPlayerState::Stopped;
}

} // namespace Sailor
