///////////////////////////////////////////////////////////////////////////////
// DirectX Sound
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// File: PlaySound.cpp
//
// Desc: DirectSound support for how to load a wave file and play it using a
//       static DirectSound buffer.
//
// Copyright (c) 1999 Microsoft Corp. All rights reserved.
//-----------------------------------------------------------------------------

#include "stdafx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <objbase.h>
#include <dsound.h>
#endif
#include "Audio/DSWaveIO.h"
#include "Engine/Object/ZzzCharacter.h"
#include "Audio/DSPlaySound.h"

#ifdef _WIN32  // ---- Windows: the DirectSound implementation ----------------

namespace
{
constexpr std::size_t kBufferNameLength = 64;

template<typename T>
struct ComReleaser
{
    void operator()(T* pointer) const noexcept
    {
        if (pointer != nullptr)
        {
            pointer->Release();
        }
    }
};

template<typename T>
using ComPtr = std::unique_ptr<T, ComReleaser<T>>;

struct SoundBufferEntry
{
    std::array<ComPtr<IDirectSoundBuffer>, MAX_CHANNEL> buffers {};
    std::array<ComPtr<IDirectSound3DBuffer>, MAX_CHANNEL> buffers3D {};
    std::array<OBJECT*, MAX_CHANNEL> attachedObjects {};
    std::array<wchar_t, kBufferNameLength> name {};
    int activeChannel = 0;
    int maxChannels = 0;
    bool enable3D = false;
    std::vector<std::uint8_t> waveData {};
    DWORD waveDataSize = 0;
};

class DirectSoundManager
{
public:
    HRESULT Initialize(HWND windowHandle);
    void Shutdown();

    void SetEnabled(bool enabled);
    bool IsEnabled() const noexcept;

    void Set3DEnabled(bool enabled);
    bool Is3DEnabled() const noexcept;

    HRESULT LoadWaveFile(ESound bufferId, const wchar_t* filename, int maxChannel, bool enable3D);
    HRESULT ReleaseBuffer(ESound bufferId);
    void ReleaseAllBuffers();

    HRESULT PlayBuffer(ESound bufferId, OBJECT* object, bool looped);
    void StopBuffer(ESound bufferId, bool resetPosition);
    void StopAll();

    HRESULT RestoreBuffers(int bufferId, int channel);
    void SetVolume(ESound bufferId, long volume);
    void SetMasterVolume(long volume);

    void Update3DPositions();

private:
    HRESULT CreateStaticBuffer(SoundBufferEntry& entry, ESound bufferId, const wchar_t* filename, bool enable3D);
    HRESULT CopyWaveDataToBuffer(SoundBufferEntry& entry, int bufferId, int channel);
    void ResetEntry(SoundBufferEntry& entry);
    void SetVolumeInternal(ESound bufferId, long volume);
    bool IsValidBufferIndex(int bufferId) const noexcept;
    bool IsValidChannelIndex(int channel) const noexcept;
    IDirectSoundBuffer* GetBuffer(int bufferId, int channel) const noexcept;
    IDirectSound3DBuffer* Get3DBuffer(int bufferId, int channel) const noexcept;
    void EnsureCoInitialized();
    void CoUninitializeIfNeeded();

    mutable std::mutex mutex_;
    ComPtr<IDirectSound> device_;
    ComPtr<IDirectSound3DListener> listener_;
    std::array<SoundBufferEntry, MAX_BUFFER> entries_ {};
    std::atomic<bool> comInitialized_ { false };
    std::uint32_t bufferBytes_ = 0;
    bool enableSound_ = false;
    bool enable3DSound_ = false;
    int loadCount_ = 0;
    long masterVolume_ = 0L;
};

DirectSoundManager& Manager()
{
    static DirectSoundManager instance;
    return instance;
}

HRESULT DirectSoundManager::Initialize(HWND windowHandle)
{
    std::lock_guard lock(mutex_);

    if (device_)
    {
        enableSound_ = true;
        return S_OK;
    }

    EnsureCoInitialized();

    IDirectSound* rawDevice = nullptr;
    HRESULT hr = DirectSoundCreate(nullptr, &rawDevice, nullptr);
    if (FAILED(hr))
    {
        g_ErrorReport.Write(L"InitDirectSound - DirectSoundCreate failed (0x%08X)\r\n", hr);
        return hr;
    }
    device_.reset(rawDevice);

    hr = device_->SetCooperativeLevel(windowHandle, DSSCL_PRIORITY);
    if (FAILED(hr))
    {
        g_ErrorReport.Write(L"InitDirectSound - SetCooperativeLevel failed (0x%08X)\r\n", hr);
        device_.reset();
        return hr;
    }

    DSBUFFERDESC desc {};
    desc.dwSize = sizeof(DSBUFFERDESC);
    desc.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_PRIMARYBUFFER;

    IDirectSoundBuffer* rawPrimary = nullptr;
    hr = device_->CreateSoundBuffer(&desc, &rawPrimary, nullptr);
    if (FAILED(hr))
    {
        g_ErrorReport.Write(L"InitDirectSound - CreateSoundBuffer failed (0x%08X)\r\n", hr);
        device_.reset();
        return hr;
    }
    ComPtr<IDirectSoundBuffer> primaryBuffer(rawPrimary);

    WAVEFORMATEX format {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 22050;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (format.wBitsPerSample / 8) * format.nChannels;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    hr = primaryBuffer->SetFormat(&format);
    if (FAILED(hr))
    {
        g_ErrorReport.Write(L"InitDirectSound - SetFormat failed (0x%08X)\r\n", hr);
        device_.reset();
        return hr;
    }

    IDirectSound3DListener* rawListener = nullptr;
    hr = primaryBuffer->QueryInterface(IID_IDirectSound3DListener, reinterpret_cast<void**>(&rawListener));
    if (FAILED(hr))
    {
        g_ErrorReport.Write(L"InitDirectSound - QueryInterface listener failed (0x%08X)\r\n", hr);
        device_.reset();
        return hr;
    }
    listener_.reset(rawListener);

    for (auto& entry : entries_)
    {
        ResetEntry(entry);
    }

    bufferBytes_ = 0;
    enableSound_ = true;
    return S_OK;
}

void DirectSoundManager::Shutdown()
{
    std::lock_guard lock(mutex_);

    ReleaseAllBuffers();
    listener_.reset();
    device_.reset();
    enableSound_ = false;
    enable3DSound_ = false;
    CoUninitializeIfNeeded();
}

void DirectSoundManager::SetEnabled(bool enabled)
{
    enableSound_ = enabled;
}

bool DirectSoundManager::IsEnabled() const noexcept
{
    return enableSound_;
}

void DirectSoundManager::Set3DEnabled(bool enabled)
{
    enable3DSound_ = enabled;
}

bool DirectSoundManager::Is3DEnabled() const noexcept
{
    return enable3DSound_;
}

HRESULT DirectSoundManager::LoadWaveFile(ESound bufferId, const wchar_t* filename, int maxChannel, bool enable3D)
{
    if (!IsEnabled())
    {
        return E_FAIL;
    }

    if (!IsValidBufferIndex(bufferId))
    {
        return E_INVALIDARG;
    }

    const int clampedChannels = std::clamp(maxChannel, 1, MAX_CHANNEL);

    std::lock_guard lock(mutex_);
    auto& entry = entries_[bufferId];
    if (entry.maxChannels > 0)
    {
        return S_FALSE;
    }

    const bool enable3DBuffer = enable3D && Is3DEnabled();
    entry.activeChannel = 0;
    entry.maxChannels = clampedChannels;
    entry.enable3D = enable3DBuffer;

    HRESULT hr = CreateStaticBuffer(entry, bufferId, filename, enable3DBuffer);
    if (FAILED(hr))
    {
        ResetEntry(entry);
        return hr;
    }
    entry.name.fill(L'\0');
    if (filename != nullptr)
    {
        wcsncpy_s(entry.name.data(), entry.name.size(), filename, _TRUNCATE);
    }

    ++loadCount_;
    SetVolumeInternal(bufferId, masterVolume_);

    return S_OK;
}

HRESULT DirectSoundManager::ReleaseBuffer(ESound bufferId)
{
    if (!IsValidBufferIndex(bufferId))
    {
        return E_INVALIDARG;
    }

    std::lock_guard lock(mutex_);
    auto& entry = entries_[bufferId];
    ResetEntry(entry);
    if (loadCount_ > 0)
    {
        --loadCount_;
    }
    return S_OK;
}

void DirectSoundManager::ReleaseAllBuffers()
{
    for (int i = 0; i < MAX_BUFFER; ++i)
    {
        ResetEntry(entries_[i]);
    }
    loadCount_ = 0;
}

HRESULT DirectSoundManager::PlayBuffer(ESound bufferId, OBJECT* object, bool looped)
{
    if (!IsEnabled())
    {
        return E_FAIL;
    }

    if (!IsValidBufferIndex(bufferId))
    {
        return E_INVALIDARG;
    }

    std::lock_guard lock(mutex_);
    auto& entry = entries_[bufferId];
    if (entry.maxChannels == 0)
    {
        return E_FAIL;
    }

    const int currentChannel = entry.activeChannel % entry.maxChannels;
    entry.activeChannel = (entry.activeChannel + 1) % entry.maxChannels;

    IDirectSoundBuffer* buffer = GetBuffer(bufferId, currentChannel);
    if (buffer == nullptr)
    {
        return E_FAIL;
    }

    HRESULT hr = RestoreBuffers(bufferId, currentChannel);
    if (FAILED(hr))
    {
        return hr;
    }

    const DWORD flags = looped ? DSBPLAY_LOOPING : 0;
    hr = buffer->Play(0, 0, flags);
    if (FAILED(hr))
    {
        g_ConsoleDebug->Write(MCD_ERROR, L"DirectSoundManager::PlayBuffer failed for %d (0x%08X)", bufferId, hr);
        return hr;
    }

    if (entry.enable3D)
    {
        entry.attachedObjects[currentChannel] = object;
    }

    return S_OK;
}

void DirectSoundManager::StopBuffer(ESound bufferId, bool resetPosition)
{
    if (!IsValidBufferIndex(bufferId))
    {
        return;
    }

    std::lock_guard lock(mutex_);
    auto& entry = entries_[bufferId];
    for (int channel = 0; channel < entry.maxChannels; ++channel)
    {
        IDirectSoundBuffer* buffer = GetBuffer(bufferId, channel);
        if (buffer == nullptr)
        {
            continue;
        }

        buffer->Stop();
        if (resetPosition)
        {
            buffer->SetCurrentPosition(0);
        }
    }
}

void DirectSoundManager::StopAll()
{
    for (int i = 0; i < MAX_BUFFER; ++i)
    {
        StopBuffer(static_cast<ESound>(i), true);
    }
}

HRESULT DirectSoundManager::RestoreBuffers(int bufferId, int channel)
{
    if (!IsValidBufferIndex(bufferId) || !IsValidChannelIndex(channel))
    {
        return E_INVALIDARG;
    }

    IDirectSoundBuffer* buffer = GetBuffer(bufferId, channel);
    if (buffer == nullptr)
    {
        return S_OK;
    }

    DWORD status = 0;
    HRESULT hr = buffer->GetStatus(&status);
    if (FAILED(hr))
    {
        return hr;
    }

    if ((status & DSBSTATUS_BUFFERLOST) == 0)
    {
        return S_OK;
    }

    do
    {
        hr = buffer->Restore();
        if (hr == DSERR_BUFFERLOST)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } while (hr == DSERR_BUFFERLOST);

    if (SUCCEEDED(hr))
    {
        auto& entry = entries_[bufferId];
        hr = CopyWaveDataToBuffer(entry, bufferId, channel);
        if (FAILED(hr))
        {
            g_ErrorReport.Write(L"RestoreBuffers - CopyWaveDataToBuffer failed for %d channel %d (0x%08X)\r\n", bufferId, channel, hr);
            return hr;
        }
    }

    return hr;
}

void DirectSoundManager::SetVolume(ESound bufferId, long volume)
{
    if (!IsValidBufferIndex(bufferId))
    {
        return;
    }

    std::lock_guard lock(mutex_);
    SetVolumeInternal(bufferId, volume);
}

void DirectSoundManager::SetMasterVolume(long volume)
{
    masterVolume_ = std::clamp<long>(volume, DSBVOLUME_MIN, DSBVOLUME_MAX);
    for (int i = 0; i < MAX_BUFFER; ++i)
    {
        SetVolume(static_cast<ESound>(i), masterVolume_);
    }
}

void DirectSoundManager::Update3DPositions()
{
    if (!IsEnabled() || !Is3DEnabled())
    {
        return;
    }

    std::lock_guard lock(mutex_);
    vec3_t angle {};
    float rotation[3][4];
    Vector(0.f, 0.f, g_Camera.Angle[2], angle);
    AngleMatrix(angle, rotation);

    for (int bufferIndex = 0; bufferIndex < MAX_BUFFER; ++bufferIndex)
    {
        auto& entry = entries_[bufferIndex];
        if (!entry.enable3D)
        {
            continue;
        }

        for (int channel = 0; channel < entry.maxChannels; ++channel)
        {
            OBJECT* object = entry.attachedObjects[channel];
            IDirectSound3DBuffer* buffer3D = Get3DBuffer(bufferIndex, channel);
            if (object == nullptr || buffer3D == nullptr)
            {
                continue;
            }

            vec3_t position;
            VectorCopy(object->Position, position);
            VectorSubtract(Hero->Object.Position, position, position);
            VectorRotate(position, rotation, position);
            VectorScale(position, 0.004f, position);
            buffer3D->SetPosition(-position[0], 0.f, -position[1], DS3D_IMMEDIATE);
        }
    }
}

HRESULT DirectSoundManager::CreateStaticBuffer(SoundBufferEntry& entry, ESound bufferId, const wchar_t* filename, bool enable3D)
{
    if (!device_)
    {
        return E_FAIL;
    }

    std::unique_ptr<waveIO> waveFile = std::make_unique<waveIO>(waveIO::Mode::Input);
    if (!waveFile->LoadWaveHeader(filename))
    {
        g_ErrorReport.Write(L"CreateStaticBuffer - Failed to load %ls\r\n", filename);
        return E_FAIL;
    }

    const WAVEFORMATEX format = waveFile->GetWaveFormatEx();
    const DWORD dataSize = waveFile->GetDataSize();
    bufferBytes_ = dataSize;

    std::vector<std::uint8_t> waveData(dataSize);
    HRESULT hr = waveFile->ReadWaveData(reinterpret_cast<char*>(waveData.data()), dataSize);
    if (FAILED(hr))
    {
        g_ErrorReport.Write(L"CreateStaticBuffer - ReadWaveData failed for %ls\r\n", filename);
        return hr;
    }

    DSBUFFERDESC desc {};
    desc.dwSize = sizeof(DSBUFFERDESC);
    desc.dwBufferBytes = dataSize;
    desc.lpwfxFormat = const_cast<WAVEFORMATEX*>(&format);

    if (enable3D)
    {
        desc.dwFlags = DSBCAPS_LOCHARDWARE | DSBCAPS_CTRL3D | DSBCAPS_STATIC;
    }
    else
    {
        desc.dwFlags = DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_STATIC;
    }

    IDirectSoundBuffer* rawBuffer = nullptr;
    hr = device_->CreateSoundBuffer(&desc, &rawBuffer, nullptr);
    if (FAILED(hr))
    {
        g_ErrorReport.Write(L"CreateStaticBuffer - CreateSoundBuffer failed for %ls (0x%08X)\r\n", filename, hr);
        return hr;
    }

    entry.buffers[0].reset(rawBuffer);
    if (enable3D)
    {
        IDirectSound3DBuffer* raw3D = nullptr;
        hr = entry.buffers[0]->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void**>(&raw3D));
        if (FAILED(hr))
        {
            g_ErrorReport.Write(L"CreateStaticBuffer - QueryInterface 3D failed for %ls (0x%08X)\r\n", filename, hr);
            entry.buffers[0].reset();
            return hr;
        }

        entry.buffers3D[0].reset(raw3D);
        entry.buffers3D[0]->SetPosition(-2.f, 0.f, 2.f, DS3D_IMMEDIATE);
    }

    entry.waveData = std::move(waveData);
    entry.waveDataSize = dataSize;

    hr = CopyWaveDataToBuffer(entry, bufferId, 0);
    if (FAILED(hr))
    {
        entry.buffers[0].reset();
        entry.buffers3D[0].reset();
        entry.waveData.clear();
        entry.waveDataSize = 0;
        return hr;
    }

    for (int channel = 1; channel < entry.maxChannels; ++channel)
    {
        IDirectSoundBuffer* duplicate = nullptr;
        hr = device_->DuplicateSoundBuffer(entry.buffers[0].get(), &duplicate);
        if (FAILED(hr))
        {
            g_ErrorReport.Write(L"CreateStaticBuffer - DuplicateSoundBuffer failed for %ls channel %d (0x%08X)\r\n", filename, channel, hr);
            return hr;
        }

        entry.buffers[channel].reset(duplicate);
        if (enable3D)
        {
            IDirectSound3DBuffer* raw3D = nullptr;
            hr = entry.buffers[channel]->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void**>(&raw3D));
            if (FAILED(hr))
            {
                g_ErrorReport.Write(L"CreateStaticBuffer - QueryInterface 3D failed for %ls channel %d (0x%08X)\r\n", filename, channel, hr);
                return hr;
            }

            entry.buffers3D[channel].reset(raw3D);
            entry.buffers3D[channel]->SetPosition(-2.f, 0.f, 2.f, DS3D_IMMEDIATE);
        }
    }

    return S_OK;
}

HRESULT DirectSoundManager::CopyWaveDataToBuffer(SoundBufferEntry& entry, int bufferId, int channel)
{
    if (entry.waveData.empty() || entry.waveDataSize == 0)
    {
        return E_FAIL;
    }

    IDirectSoundBuffer* buffer = GetBuffer(bufferId, channel);
    if (buffer == nullptr)
    {
        return E_FAIL;
    }

    std::uint8_t* part1 = nullptr;
    std::uint8_t* part2 = nullptr;
    DWORD part1Size = 0;
    DWORD part2Size = 0;
    HRESULT hr = buffer->Lock(0, entry.waveDataSize, reinterpret_cast<void**>(&part1), &part1Size,
        reinterpret_cast<void**>(&part2), &part2Size, 0);
    if (FAILED(hr))
    {
        return hr;
    }

    std::memcpy(part1, entry.waveData.data(), part1Size);
    if (part2 != nullptr && part2Size > 0)
    {
        std::memcpy(part2, entry.waveData.data() + part1Size, part2Size);
    }

    buffer->Unlock(part1, part1Size, part2, part2Size);
    return S_OK;
}

void DirectSoundManager::ResetEntry(SoundBufferEntry& entry)
{
    for (auto& buffer : entry.buffers)
    {
        buffer.reset();
    }
    for (auto& buffer3D : entry.buffers3D)
    {
        buffer3D.reset();
    }
    entry.attachedObjects.fill(nullptr);
    entry.name.fill(L'\0');
    entry.activeChannel = 0;
    entry.maxChannels = 0;
    entry.enable3D = false;
    entry.waveData.clear();
    entry.waveDataSize = 0;
}

void DirectSoundManager::SetVolumeInternal(ESound bufferId, long volume)
{
    // NOTE: Assumes caller holds the mutex.
    const long clamped = std::clamp<long>(volume, DSBVOLUME_MIN, DSBVOLUME_MAX);
    auto& entry = entries_[bufferId];
    for (int channel = 0; channel < entry.maxChannels; ++channel)
    {
        IDirectSoundBuffer* buffer = entry.buffers[channel].get();
        if (buffer != nullptr)
        {
            buffer->SetVolume(clamped);
        }
    }
}

bool DirectSoundManager::IsValidBufferIndex(int bufferId) const noexcept
{
    return bufferId >= 0 && bufferId < MAX_BUFFER;
}

bool DirectSoundManager::IsValidChannelIndex(int channel) const noexcept
{
    return channel >= 0 && channel < MAX_CHANNEL;
}

IDirectSoundBuffer* DirectSoundManager::GetBuffer(int bufferId, int channel) const noexcept
{
    if (!IsValidBufferIndex(bufferId) || !IsValidChannelIndex(channel))
    {
        return nullptr;
    }

    return entries_[bufferId].buffers[channel].get();
}

IDirectSound3DBuffer* DirectSoundManager::Get3DBuffer(int bufferId, int channel) const noexcept
{
    if (!IsValidBufferIndex(bufferId) || !IsValidChannelIndex(channel))
    {
        return nullptr;
    }

    return entries_[bufferId].buffers3D[channel].get();
}

void DirectSoundManager::EnsureCoInitialized()
{
    if (!comInitialized_.exchange(true))
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
}

void DirectSoundManager::CoUninitializeIfNeeded()
{
    if (comInitialized_.exchange(false))
    {
        CoUninitialize();
    }
}
} // namespace

HRESULT InitDirectSound(HWND windowHandle)
{
    return Manager().Initialize(windowHandle);
}

void SetEnableSound(bool enabled)
{
    Manager().SetEnabled(enabled);
    if (!enabled)
    {
        Manager().StopAll();
    }
}

void FreeDirectSound()
{
    Manager().Shutdown();
}

void LoadWaveFile(ESound bufferId, const wchar_t* filename, int maxChannel, bool enable3D)
{
    Manager().LoadWaveFile(bufferId, filename, maxChannel, enable3D);
}

HRESULT ReleaseBuffer(int bufferId)
{
    return Manager().ReleaseBuffer(static_cast<ESound>(bufferId));
}

HRESULT RestoreBuffers(int bufferId, int channel)
{
    return Manager().RestoreBuffers(bufferId, channel);
}

HRESULT PlayBuffer(ESound bufferId, OBJECT* object, BOOL looped)
{
    return Manager().PlayBuffer(bufferId, object, looped != FALSE);
}

VOID StopBuffer(ESound bufferId, BOOL resetPosition)
{
    Manager().StopBuffer(bufferId, resetPosition != FALSE);
}

void AllStopSound()
{
    Manager().StopAll();
}

void SetVolume(int bufferId, long volume)
{
    Manager().SetVolume(static_cast<ESound>(bufferId), volume);
}

void SetMasterVolume(long volume)
{
    Manager().SetMasterVolume(volume);
}

void Set3DSoundPosition()
{
    Manager().Update3DPositions();
}

#else  // ---- non-Windows: SDL3_mixer backend -------------------------------

// Sound effects on SDL3_mixer, mirroring the DirectSound semantics above so no
// call site changes: LoadWaveFile registers a buffer, PlayBuffer starts it on a
// free voice, and 3D sounds are panned from the hero's position each frame.
//
// The WAV decoding is deliberately the SAME code the Windows build uses -
// waveIO sits on the portable mmio shim and already resolves paths - so both
// platforms agree on which files exist and how they are read. Only the
// playback device differs.
//
// One mixer device for the whole process, borrowed from AudioPlayer (music):
// SDL mixes everything itself, and a second device would contend for the
// hardware.

#include "Audio/AudioPlayer.h"
#include "Engine/Object/ZzzObject.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

extern CHARACTER* Hero;   // the listener sits on the hero (Hero->Object.Position)

namespace
{
    // Distance at which a positional sound fades to silence, in world units.
    // Matches the DirectSound listener's max distance closely enough that
    // ambience keeps the same reach it has on Windows.
    constexpr float k3DMaxDistance = 1500.0f;

    struct SfxBuffer
    {
        MIX_Audio* audio = nullptr;
        std::vector<MIX_Track*> voices;   // one per simultaneous playback
        bool is3D = false;
        float gain = 1.0f;                // per-buffer volume (SetVolume)
        OBJECT* source[8] = {};           // world object driving each voice
    };

    std::array<SfxBuffer, MAX_BUFFER> g_buffers;
    bool g_enabled = true;
    float g_masterGain = 1.0f;

    MIX_Mixer* Mixer() { return AudioPlayer::Mixer(); }

    // Wrap the decoded PCM in the WAV container SDL expects, so the bytes
    // waveIO produced can be handed to MIX_LoadAudio_IO without a temp file.
    std::vector<Uint8> BuildWavImage(const WAVEFORMATEX& fmt, const std::vector<Uint8>& pcm)
    {
        const Uint32 dataSize = static_cast<Uint32>(pcm.size());
        std::vector<Uint8> wav;
        wav.reserve(44 + pcm.size());

        auto put32 = [&wav](Uint32 v) {
            wav.push_back(static_cast<Uint8>(v & 0xFF));
            wav.push_back(static_cast<Uint8>((v >> 8) & 0xFF));
            wav.push_back(static_cast<Uint8>((v >> 16) & 0xFF));
            wav.push_back(static_cast<Uint8>((v >> 24) & 0xFF));
        };
        auto put16 = [&wav](Uint16 v) {
            wav.push_back(static_cast<Uint8>(v & 0xFF));
            wav.push_back(static_cast<Uint8>((v >> 8) & 0xFF));
        };
        auto puts = [&wav](const char* s) { for (int i = 0; i < 4; ++i) wav.push_back(static_cast<Uint8>(s[i])); };

        puts("RIFF"); put32(36 + dataSize); puts("WAVE");
        puts("fmt "); put32(16);
        put16(static_cast<Uint16>(fmt.wFormatTag));
        put16(static_cast<Uint16>(fmt.nChannels));
        put32(static_cast<Uint32>(fmt.nSamplesPerSec));
        put32(static_cast<Uint32>(fmt.nAvgBytesPerSec));
        put16(static_cast<Uint16>(fmt.nBlockAlign));
        put16(static_cast<Uint16>(fmt.wBitsPerSample));
        puts("data"); put32(dataSize);
        wav.insert(wav.end(), pcm.begin(), pcm.end());
        return wav;
    }

    // A voice that is free (never started, or finished playing).
    MIX_Track* FreeVoice(SfxBuffer& buf, int& outIndex)
    {
        for (size_t i = 0; i < buf.voices.size(); ++i)
        {
            if (!MIX_TrackPlaying(buf.voices[i]))
            {
                outIndex = static_cast<int>(i);
                return buf.voices[i];
            }
        }
        // All voices busy: reuse the first, the way DirectSound's buffer
        // duplication runs out and restarts the oldest.
        outIndex = 0;
        return buf.voices.empty() ? nullptr : buf.voices[0];
    }
}

HRESULT InitDirectSound(HWND)
{
    // AudioPlayer::Initialize already opened the device; nothing else to do.
    return Mixer() ? S_OK : E_FAIL;
}

void SetEnableSound(bool b)
{
    g_enabled = b;
    if (!b) AllStopSound();
}

void FreeDirectSound()
{
    for (SfxBuffer& buf : g_buffers)
    {
        for (MIX_Track* voice : buf.voices)
        {
            if (voice) { MIX_StopTrack(voice, 0); MIX_DestroyTrack(voice); }
        }
        buf.voices.clear();
        if (buf.audio) { MIX_DestroyAudio(buf.audio); buf.audio = nullptr; }
    }
}

void LoadWaveFile(ESound Buffer, const wchar_t* strFileName, int BufferChannel, bool Enable3DSound)
{
    if (Buffer < 0 || Buffer >= MAX_BUFFER || !Mixer()) return;

    SfxBuffer& buf = g_buffers[Buffer];
    if (buf.audio) return;                      // already loaded

    auto waveFile = std::make_unique<waveIO>(waveIO::Mode::Input);
    if (!waveFile->LoadWaveHeader(strFileName)) return;

    const WAVEFORMATEX format = waveFile->GetWaveFormatEx();
    const int dataSize = waveFile->GetDataSize();
    if (dataSize <= 0) return;

    std::vector<Uint8> pcm(static_cast<size_t>(dataSize));
    if (!waveFile->ReadWaveData(reinterpret_cast<char*>(pcm.data()), dataSize)) return;

    const std::vector<Uint8> wav = BuildWavImage(format, pcm);
    SDL_IOStream* io = SDL_IOFromConstMem(wav.data(), wav.size());
    if (!io) return;

    // predecode=true: these are short effects played repeatedly, and decoding
    // once up front keeps the mixer callback free of file work.
    buf.audio = MIX_LoadAudio_IO(Mixer(), io, /*predecode=*/true, /*closeio=*/true);
    if (!buf.audio) return;

    buf.is3D = Enable3DSound;
    const int voices = std::clamp(BufferChannel, 1, 8);
    for (int i = 0; i < voices; ++i)
    {
        MIX_Track* track = MIX_CreateTrack(Mixer());
        if (!track) break;
        MIX_SetTrackAudio(track, buf.audio);
        buf.voices.push_back(track);
    }
}

HRESULT PlayBuffer(ESound Buffer, OBJECT* Object, BOOL bLooped)
{
    if (!g_enabled || Buffer < 0 || Buffer >= MAX_BUFFER) return S_OK;

    SfxBuffer& buf = g_buffers[Buffer];
    if (!buf.audio || buf.voices.empty()) return S_OK;

    int index = 0;
    MIX_Track* voice = FreeVoice(buf, index);
    if (!voice) return S_OK;

    buf.source[index] = buf.is3D ? Object : nullptr;
    MIX_SetTrackGain(voice, buf.gain * g_masterGain);
    // -1 loops forever, 0 plays once - the same two cases the callers use.
    MIX_PlayTrack(voice, bLooped ? -1 : 0);
    return S_OK;
}

void StopBuffer(ESound Buffer, BOOL)
{
    if (Buffer < 0 || Buffer >= MAX_BUFFER) return;
    for (MIX_Track* voice : g_buffers[Buffer].voices)
        if (voice) MIX_StopTrack(voice, 0);
}

void AllStopSound(void)
{
    for (SfxBuffer& buf : g_buffers)
        for (MIX_Track* voice : buf.voices)
            if (voice) MIX_StopTrack(voice, 0);
}

void Set3DSoundPosition()
{
    if (!Hero) return;

    for (SfxBuffer& buf : g_buffers)
    {
        if (!buf.is3D) continue;
        for (size_t i = 0; i < buf.voices.size(); ++i)
        {
            MIX_Track* voice = buf.voices[i];
            OBJECT* src = buf.source[i];
            if (!voice || !src || !MIX_TrackPlaying(voice)) continue;

            // Relative to the hero, exactly as the DirectSound path measures
            // it (the listener sits on the hero, not the camera).
            // World units are large; scale into the mixer's space and let it
            // handle attenuation and panning from there.
            const float dx = src->Position[0] - Hero->Object.Position[0];
            const float dy = src->Position[1] - Hero->Object.Position[1];
            const float dz = src->Position[2] - Hero->Object.Position[2];
            const MIX_Point3D pos = {
                dx / k3DMaxDistance * 10.0f,
                dz / k3DMaxDistance * 10.0f,
                dy / k3DMaxDistance * 10.0f,
            };
            MIX_SetTrack3DPosition(voice, &pos);
        }
    }
}

HRESULT ReleaseBuffer(int Buffer)
{
    if (Buffer < 0 || Buffer >= MAX_BUFFER) return S_OK;
    SfxBuffer& buf = g_buffers[Buffer];
    for (MIX_Track* voice : buf.voices)
    {
        if (voice) { MIX_StopTrack(voice, 0); MIX_DestroyTrack(voice); }
    }
    buf.voices.clear();
    if (buf.audio) { MIX_DestroyAudio(buf.audio); buf.audio = nullptr; }
    return S_OK;
}

HRESULT RestoreBuffers(int, int)
{
    // DirectSound buffers could be lost when another app took the device;
    // SDL owns mixing here, so there is nothing to restore.
    return S_OK;
}

void SetVolume(int Buffer, long vol)
{
    if (Buffer < 0 || Buffer >= MAX_BUFFER) return;
    // The engine passes hundredths of a decibel of attenuation (0 = full,
    // -10000 = silence), which is what DirectSound wanted.
    const float gain = std::clamp(SDL_powf(10.0f, static_cast<float>(vol) / 2000.0f), 0.0f, 1.0f);
    SfxBuffer& buf = g_buffers[Buffer];
    buf.gain = gain;
    for (MIX_Track* voice : buf.voices)
        if (voice) MIX_SetTrackGain(voice, gain * g_masterGain);
}

void SetMasterVolume(long vol)
{
    g_masterGain = std::clamp(SDL_powf(10.0f, static_cast<float>(vol) / 2000.0f), 0.0f, 1.0f);
    for (SfxBuffer& buf : g_buffers)
        for (MIX_Track* voice : buf.voices)
            if (voice) MIX_SetTrackGain(voice, buf.gain * g_masterGain);
}

#endif // _WIN32
