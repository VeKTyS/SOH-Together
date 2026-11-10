// Voice chat, louder when they are close (Crests of Courage voice.cpp / voice_io.cpp)
//
// How it goes:
// - our mic is read with SDL2 (its own capture device, not the game's audio),
//   cut in 20 ms chunks, packed with Opus and sent on the fast lane (lost = skipped)
// - only sent while we talk (+ a bit before / after), like CoC
// - their chunks are unpacked and mixed into our own SDL2 output device,
//   quieter the farther their Link is, panned left/right from the camera
// Opus and SDL2 are already in soh.exe (SoH's vcpkg), nothing new to link.
#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <spdlog/spdlog.h>

#include <SDL2/SDL.h>
#include <opus.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}
// libultra's r4300.h has a U32(x) macro that breaks ByteWriter::U32 (same fix as CoopEnemies.cpp)
#undef U32

namespace {

using Clock = std::chrono::steady_clock;

// --- audio settings, copied from CoC voice_io.cpp ---
constexpr int kRate = 48000;
constexpr int kFrame = 960; // 20 ms
constexpr int kMaxOpus = 400;
constexpr int kBitrate = 16000;
constexpr float kLoudRms = 0.012f; // louder than this = we're talking
// CoC waits 2 frames (40 ms) before playing someone. Their game ticks every 33 ms, ours every
// 50 ms and frames arrive in bunches once per tick, so we wait one more frame.
constexpr int kLeadFrames = 3;
constexpr int kMaxBehindFrames = 12; // more queued than that = we're late, drop it
constexpr int kMaxConcealed = 2;     // lost frames we try to hide (Opus guesses them)
const char* const kDefaultMic = "Default Microphone";

// --- copied from CoC voice.cpp / mod.hpp ---
constexpr size_t kVoiceMaxOpus = 256; // biggest frame we send or accept
constexpr int kMaxHeard = 8;          // at most 8 people heard at once
constexpr int kPreroll = 2;           // frames kept from just before we started talking
constexpr auto kHangover = std::chrono::milliseconds(400);
constexpr auto kSpeakingShown = std::chrono::milliseconds(550);
constexpr auto kHeardFor = std::chrono::milliseconds(500);

// CoC distances are in Twilight Princess units. Link is ~140 tall there, ~55 here,
// so 1 TP unit = 0.4 OoT unit (CoC's 4000 = about 50 m in both games).
constexpr float kTpToOot = 0.4f;

#define VOICE_CVAR(name) CVAR_COOP("Voice." name)

// CoC's defaults: off, fade on, range 50%, volumes 100%, key M
bool Enabled() {
    return CVarGetInteger(VOICE_CVAR("Enabled"), 0) != 0;
}
bool MicMuted() {
    return CVarGetInteger(VOICE_CVAR("Muted"), 0) != 0;
}
bool Proximity() {
    return CVarGetInteger(VOICE_CVAR("Proximity"), 1) != 0;
}
int RangePercent() {
    return std::clamp(CVarGetInteger(VOICE_CVAR("Range"), 50), 0, 200);
}
int MicVolume() {
    return std::clamp(CVarGetInteger(VOICE_CVAR("MicVolume"), 100), 0, 200);
}
int PlayerVolume() {
    return std::clamp(CVarGetInteger(VOICE_CVAR("PlayerVolume"), 100), 0, 200);
}
std::string CaptureDevice() {
    std::string input = CVarGetString(VOICE_CVAR("Microphone"), kDefaultMic);
    if (input == kDefaultMic) {
        input.clear();
    }
    return input;
}

bool IsKeyboardKey(int key) {
    return key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_GamepadStart;
}
ImGuiKey MuteKey() {
    int key = CVarGetInteger(VOICE_CVAR("MuteKey"), ImGuiKey_M);
    return IsKeyboardKey(key) ? static_cast<ImGuiKey>(key) : ImGuiKey_M;
}
std::string MuteKeyName() {
    return ImGui::GetCurrentContext() != nullptr ? ImGui::GetKeyName(MuteKey()) : "M";
}

void SaveSettings() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

// SoH may play its sound with WASAPI, so SDL's audio may not be started. Starting it
// again is fine, SDL counts. Only done once someone turns voice chat on.
bool SdlAudioReady() {
    static bool sTried = false;
    static bool sOk = false;
    if (!sTried) {
        sTried = true;
        sOk = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
        if (!sOk) {
            SPDLOG_WARN("[Coop] voice: SDL audio didn't start: {}", SDL_GetError());
        }
    }
    return sOk;
}

// ---------------------------------------------------------------------------
// Device: mic + speakers + Opus (copied from CoC voice_io.cpp, SDL3 -> SDL2)
// ---------------------------------------------------------------------------

struct Frame {
    uint32_t seq = 0;
    std::vector<uint8_t> opus;
    bool loud = false;
};

// One queue of stereo samples per player, mixed together when SDL asks for sound.
// Copied from CoC's Mixer (the one they use where they can't bind one stream per player).
class Mixer {
  public:
    virtual ~Mixer() = default;

    void Add(uint32_t who) {
        std::lock_guard lock(mMutex);
        mLanes[who];
    }
    void Remove(uint32_t who) {
        std::lock_guard lock(mMutex);
        mLanes.erase(who);
    }
    void Put(uint32_t who, const int16_t* stereo, int frames) {
        std::lock_guard lock(mMutex);
        auto it = mLanes.find(who);
        if (it != mLanes.end()) {
            it->second.buf.insert(it->second.buf.end(), stereo, stereo + frames * 2);
        }
    }
    int Queued(uint32_t who) {
        std::lock_guard lock(mMutex);
        auto it = mLanes.find(who);
        return it != mLanes.end() ? static_cast<int>(it->second.buf.size() / 2) : 0;
    }
    void Clear(uint32_t who) {
        std::lock_guard lock(mMutex);
        auto it = mLanes.find(who);
        if (it != mLanes.end()) {
            it->second.buf.clear();
            it->second.frac = 0.0;
        }
    }
    void SetGain(float gain) {
        std::lock_guard lock(mMutex);
        mGain = gain;
    }

    // SDL's audio thread: fill n stereo samples. rate != kRate would stretch (CoC's resampling),
    // SDL2 converts for us here so it's always kRate.
    void Pull(int16_t* out, int n, int rate) {
        std::lock_guard lock(mMutex);
        const double step = rate > 0 ? static_cast<double>(kRate) / rate : 1.0;
        for (int i = 0; i < n; ++i) {
            float left = 0.0f, right = 0.0f;
            for (auto& [who, lane] : mLanes) {
                if (lane.buf.size() < 2) {
                    continue;
                }
                const bool next = lane.buf.size() >= 4;
                const float t = static_cast<float>(lane.frac);
                const float l0 = lane.buf[0], r0 = lane.buf[1];
                const float l1 = next ? lane.buf[2] : l0, r1 = next ? lane.buf[3] : r0;
                left += l0 + (l1 - l0) * t;
                right += r0 + (r1 - r0) * t;
                lane.frac += step;
                while (lane.frac >= 1.0 && lane.buf.size() >= 2) {
                    lane.buf.pop_front();
                    lane.buf.pop_front();
                    lane.frac -= 1.0;
                }
            }
            out[i * 2] = static_cast<int16_t>(std::clamp(left * mGain, -32768.0f, 32767.0f));
            out[i * 2 + 1] = static_cast<int16_t>(std::clamp(right * mGain, -32768.0f, 32767.0f));
        }
    }

  private:
    struct Lane {
        std::deque<int16_t> buf;
        double frac = 0.0;
    };
    std::mutex mMutex;
    std::map<uint32_t, Lane> mLanes;
    float mGain = 1.0f;
};

// Our own SDL2 output device (not the game's), pulling from the mixer.
class Speakers : public Mixer {
  public:
    bool Open() {
        SDL_AudioSpec want{};
        want.freq = kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = &Speakers::Fill;
        want.userdata = this;
        SDL_AudioSpec have{};
        // 0 = no changes allowed: SDL converts to the real device format itself
        mDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (mDevice == 0) {
            return false;
        }
        SDL_PauseAudioDevice(mDevice, 0);
        return true;
    }
    ~Speakers() override {
        // closing waits for the callback to finish, so the mixer is still alive here
        if (mDevice != 0) {
            SDL_CloseAudioDevice(mDevice);
        }
    }

  private:
    static void SDLCALL Fill(void* user, Uint8* stream, int len) {
        static_cast<Speakers*>(user)->Pull(reinterpret_cast<int16_t*>(stream), len / 4, kRate);
    }
    SDL_AudioDeviceID mDevice = 0;
};

struct Decoder {
    OpusDecoder* opus = nullptr;
    uint32_t lastSeq = 0;
};

class Device {
  public:
    ~Device() {
        Close();
    }

    std::vector<std::string> Microphones() {
        std::vector<std::string> out{ kDefaultMic };
        if (!SdlAudioReady()) {
            return out;
        }
        int count = SDL_GetNumAudioDevices(1);
        for (int i = 0; i < count; ++i) {
            const char* n = SDL_GetAudioDeviceName(i, 1);
            if (n != nullptr && n[0] != '\0' && std::string(n) != kDefaultMic) {
                out.emplace_back(n);
            }
        }
        return out;
    }

    void Set(bool on, bool micOn, const std::string& mic, float micGain, float outGain) {
        mMicGain = std::clamp(micGain, 0.0f, 2.0f);
        mOutGain = std::clamp(outGain, 0.0f, 2.0f);
        if (!on) {
            if (mOn) {
                Close();
            }
            mOn = false;
            mProblem.clear();
            return;
        }
        if (!SdlAudioReady()) {
            mProblem = "Voice isn't available here";
            return;
        }
        mOn = true;
        mMic = mic;
        const auto now = Clock::now();
        // mic failed: try again every 3 s. Muted = mic closed for real.
        const bool retry = micOn && mCapture == 0 && now - mLastOpen > std::chrono::seconds(3);
        if (mMicOn != micOn || retry || (micOn && mCapture != 0 && mCaptureMic != mic)) {
            mMicOn = micOn;
            mProblem.clear();
            if (micOn) {
                OpenCapture();
            } else {
                CloseCapture();
            }
        }
        if (!mSpeakers && now - mLastSpeakers > std::chrono::seconds(3)) {
            mLastSpeakers = now;
            mSpeakers = std::make_unique<Speakers>();
            if (!mSpeakers->Open()) {
                mSpeakers.reset();
                mProblem = "Couldn't open the speakers";
            }
        }
        if (mSpeakers) {
            mSpeakers->SetGain(mOutGain);
        }
    }

    // What the mic got since last time, packed in Opus frames.
    std::vector<Frame> Take() {
        std::vector<Frame> out;
        if (mCapture == 0 || mEncoder == nullptr) {
            return out;
        }
        Uint32 bytes = SDL_GetQueuedAudioSize(mCapture);
        while (bytes >= 2) {
            int16_t chunk[kFrame];
            const Uint32 want = std::min<Uint32>(bytes, sizeof(chunk)) & ~1u;
            const Uint32 got = SDL_DequeueAudio(mCapture, chunk, want);
            if (got == 0) {
                break;
            }
            mPending.insert(mPending.end(), chunk, chunk + got / 2);
            bytes -= std::min(bytes, got);
        }
        size_t at = 0;
        while (mPending.size() - at >= static_cast<size_t>(kFrame)) {
            int16_t pcm[kFrame];
            double energy = 0.0;
            for (int i = 0; i < kFrame; ++i) {
                const float v = std::clamp(static_cast<float>(mPending[at + i]) * mMicGain, -32768.0f, 32767.0f);
                pcm[i] = static_cast<int16_t>(v);
                const double n = v / 32768.0;
                energy += n * n;
            }
            at += kFrame;
            Frame f;
            f.opus.resize(kMaxOpus);
            const int n = opus_encode(mEncoder, pcm, kFrame, f.opus.data(), kMaxOpus);
            if (n <= 0) {
                continue;
            }
            f.opus.resize(static_cast<size_t>(n));
            f.seq = ++mSeq;
            f.loud = std::sqrt(energy / kFrame) > kLoudRms;
            out.push_back(std::move(f));
        }
        mPending.erase(mPending.begin(), mPending.begin() + static_cast<std::ptrdiff_t>(at));
        if (mPending.size() > static_cast<size_t>(kFrame * 10)) {
            mPending.clear();
        }
        return out;
    }

    // Someone's frame: unpack it and queue it, gain 0..1, pan -1 (left) .. 1 (right).
    void Play(uint32_t who, uint32_t seq, const uint8_t* opus, size_t size, float gain, float pan) {
        if (!mOn || !mSpeakers || opus == nullptr || size == 0 || size > kMaxOpus) {
            return;
        }
        auto found = mVoices.find(who);
        if (found == mVoices.end()) {
            int err = 0;
            Decoder d;
            d.opus = opus_decoder_create(kRate, 1, &err);
            if (d.opus == nullptr) {
                return;
            }
            mSpeakers->Add(who);
            found = mVoices.emplace(who, d).first;
        }
        Decoder& v = found->second;

        // old or repeated frame (the lane can reorder): skip it
        if (v.lastSeq != 0 && static_cast<int32_t>(seq - v.lastSeq) <= 0) {
            return;
        }
        const uint32_t missing = v.lastSeq == 0 ? 0 : seq - v.lastSeq - 1;
        v.lastSeq = seq;

        const int queued = mSpeakers->Queued(who);
        if (queued > kMaxBehindFrames * kFrame) {
            mSpeakers->Clear(who);
        }
        if (queued <= 0) {
            // start of a sentence: a bit of silence first so the next frames have time to arrive
            static const int16_t kSilence[kFrame * kLeadFrames * 2] = {};
            mSpeakers->Put(who, kSilence, kFrame * kLeadFrames);
        }

        const float level = std::clamp(gain, 0.0f, 1.0f);
        const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.78539816f;
        const float toLeft = level * std::clamp(std::cos(angle) * 1.41421356f, 0.2f, 1.0f);
        const float toRight = level * std::clamp(std::sin(angle) * 1.41421356f, 0.2f, 1.0f);
        int16_t pcm[kFrame];
        int16_t stereo[kFrame * 2];
        const auto put = [&](int samples) {
            if (samples <= 0) {
                return;
            }
            for (int i = 0; i < samples; ++i) {
                const float s = static_cast<float>(pcm[i]);
                stereo[i * 2] = static_cast<int16_t>(s * toLeft);
                stereo[i * 2 + 1] = static_cast<int16_t>(s * toRight);
            }
            mSpeakers->Put(who, stereo, samples);
        };
        if (missing > 0 && missing <= static_cast<uint32_t>(kMaxConcealed) + 1) {
            // a few frames got lost: let Opus fill the gap (the last one from this frame's backup copy)
            for (uint32_t i = 1; i < missing; ++i) {
                put(opus_decode(v.opus, nullptr, 0, pcm, kFrame, 0));
            }
            put(opus_decode(v.opus, opus, static_cast<opus_int32>(size), pcm, kFrame, 1));
        }
        put(opus_decode(v.opus, opus, static_cast<opus_int32>(size), pcm, kFrame, 0));
    }

    void Forget(uint32_t who) {
        auto it = mVoices.find(who);
        if (it == mVoices.end()) {
            return;
        }
        if (it->second.opus != nullptr) {
            opus_decoder_destroy(it->second.opus);
        }
        if (mSpeakers) {
            mSpeakers->Remove(who);
        }
        mVoices.erase(it);
    }

    std::vector<uint32_t> Voices() const {
        std::vector<uint32_t> out;
        for (const auto& [who, d] : mVoices) {
            out.push_back(who);
        }
        return out;
    }

    void Close() {
        CloseCapture();
        if (mEncoder != nullptr) {
            opus_encoder_destroy(mEncoder);
        }
        mEncoder = nullptr;
        while (!mVoices.empty()) {
            Forget(mVoices.begin()->first);
        }
        mSpeakers.reset();
        mMicOn = false;
    }

    const std::string& Problem() const {
        return mProblem;
    }

  private:
    void CloseCapture() {
        if (mCapture != 0) {
            SDL_CloseAudioDevice(mCapture);
        }
        mCapture = 0;
        mCaptureMic.clear();
        mPending.clear();
    }

    void OpenCapture() {
        CloseCapture();
        mLastOpen = Clock::now();
        if (mEncoder == nullptr) {
            int err = 0;
            mEncoder = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
            if (mEncoder == nullptr) {
                mProblem = "Couldn't start the voice encoder";
                return;
            }
            opus_encoder_ctl(mEncoder, OPUS_SET_BITRATE(kBitrate));
            opus_encoder_ctl(mEncoder, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(mEncoder, OPUS_SET_PACKET_LOSS_PERC(10));
        }
        // the saved mic isn't plugged in: use the default one (CoC's mic_id)
        std::string name;
        if (!mMic.empty() && mMic != kDefaultMic) {
            for (const std::string& m : Microphones()) {
                if (m == mMic) {
                    name = m;
                }
            }
        }
        SDL_AudioSpec want{};
        want.freq = kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = kFrame;
        want.callback = nullptr; // no callback = SDL queues it, we read it with SDL_DequeueAudio
        SDL_AudioSpec have{};
        mCapture = SDL_OpenAudioDevice(name.empty() ? nullptr : name.c_str(), 1, &want, &have, 0);
        if (mCapture == 0) {
            mProblem = "No microphone found";
            return;
        }
        SDL_PauseAudioDevice(mCapture, 0);
        mCaptureMic = mMic;
    }

    std::string mProblem;
    bool mOn = false;
    bool mMicOn = false;
    std::string mMic;
    float mMicGain = 1.0f;
    float mOutGain = 1.0f;

    std::unique_ptr<Speakers> mSpeakers;
    Clock::time_point mLastSpeakers{};
    std::map<uint32_t, Decoder> mVoices;

    SDL_AudioDeviceID mCapture = 0;
    std::string mCaptureMic;
    Clock::time_point mLastOpen{};
    OpusEncoder* mEncoder = nullptr;
    std::vector<int16_t> mPending;
    uint32_t mSeq = 0;
};

// ---------------------------------------------------------------------------
// Voice chat logic (copied from CoC voice.cpp)
// ---------------------------------------------------------------------------

std::unique_ptr<Device> sVoice;
std::string sStatus = "Off";
std::vector<std::string> sDevices;
Clock::time_point sDevicesAt{};
bool sKeyCapture = false;
bool sKeyWasDown = false;
bool sHeldAtCapture[ImGuiKey_NamedKey_COUNT] = {};
bool sMuteKnown = false;
bool sMuteLast = false;

std::deque<Frame> sPreroll;
Clock::time_point sTalkingUntil{};
Clock::time_point sLocalSpeakingUntil{};

struct Speaker {
    Clock::time_point shownUntil{};
    Clock::time_point heardUntil{};
};
std::map<uint32_t, Speaker> sSpeakers;

// CoC: voice on + we're connected. Hosting alone doesn't count (mic stays closed).
bool Live() {
    return Enabled() && CoopGame::SessionActive() && CoopGame::SelfId() != 0 &&
           !CoopGame::Session().Players().empty();
}

float FadeDistance() {
    return 4000.0f * kTpToOot * static_cast<float>(RangePercent()) / 50.0f;
}

// full volume up to a quarter of the range, then a smooth fade to 0 at the edge
float GainAt(float distance) {
    const float edge = FadeDistance();
    if (edge <= 0.0f || !std::isfinite(distance) || distance >= edge) {
        return 0.0f;
    }
    const float full = edge * 0.25f;
    if (distance <= full) {
        return 1.0f;
    }
    const float t = 1.0f - (distance - full) / (edge - full);
    return t * t * (3.0f - 2.0f * t);
}

// CoC's co-op path: fade off = everyone at full volume, else same scene + distance
float GainFor(const Coop::RemotePlayer& p) {
    if (!Proximity()) {
        return 1.0f;
    }
    const Coop::PlayerState& me = CoopGame::LocalState();
    if (!me.inGame || !p.state || !p.state->inGame || p.state->sceneNum != me.sceneNum) {
        return 0.0f;
    }
    const float dx = p.state->x - me.x, dy = p.state->y - me.y, dz = p.state->z - me.z;
    return GainAt(std::sqrt(dx * dx + dy * dy + dz * dz));
}

// left/right from where the camera looks (CoC pan_for)
float PanFor(const Coop::RemotePlayer& p) {
    if (gPlayState == nullptr || !p.state) {
        return 0.0f;
    }
    const Vec3f eye = gPlayState->view.eye;
    const Vec3f center = gPlayState->view.lookAt;
    float fx = center.x - eye.x, fz = center.z - eye.z;
    const float dx = p.state->x - eye.x, dz = p.state->z - eye.z;
    const float flen = std::sqrt(fx * fx + fz * fz), dlen = std::sqrt(dx * dx + dz * dz);
    if (!(flen > 0.001f) || !(dlen > 0.001f)) {
        return 0.0f;
    }
    fx /= flen;
    fz /= flen;
    float pan = (dx * -fz + dz * fx) / dlen;
    // right next to us = both ears
    const float closeness = std::clamp(dlen / (400.0f * kTpToOot), 0.0f, 1.0f);
    pan *= closeness;
    if (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0)) {
        pan = -pan;
    }
    return std::clamp(pan, -1.0f, 1.0f);
}

// Event data: u32 sequence, u16 size, then the Opus bytes
void SendFrame(const Frame& frame) {
    if (frame.opus.empty() || frame.opus.size() > kVoiceMaxOpus) {
        return;
    }
    Coop::ByteWriter w;
    w.U32(frame.seq);
    w.U16(static_cast<uint16_t>(frame.opus.size()));
    w.Bytes(frame.opus);
    CoopGame::SendEvent(Coop::EventKind::Voice, w.Take(), Coop::Channel::Snapshot);
}

bool IsModifier(int key) {
    return key == ImGuiKey_LeftCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_LeftAlt ||
           key == ImGuiKey_LeftSuper || key == ImGuiKey_RightCtrl || key == ImGuiKey_RightShift ||
           key == ImGuiKey_RightAlt || key == ImGuiKey_RightSuper;
}

// CoC reads the keyboard with GetAsyncKeyState; we ask ImGui, which gets the keys from
// SoH's window on every backend (DX11 / OpenGL) and forgets them when the game loses focus.
void UpdateMuteKey() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    if (sKeyCapture) {
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart; ++key) {
            const int slot = key - ImGuiKey_NamedKey_BEGIN;
            const bool down = ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
            if (!down) {
                sHeldAtCapture[slot] = false;
                continue;
            }
            if (sHeldAtCapture[slot]) {
                continue; // was already held when we started listening
            }
            if (key == ImGuiKey_Escape) {
                sKeyCapture = false;
                CoopGame::Notify(CoopGame::NoticeKind::Other, "Mute key unchanged");
                return;
            }
            if (IsModifier(key)) {
                continue;
            }
            CVarSetInteger(VOICE_CVAR("MuteKey"), key);
            SaveSettings();
            sKeyCapture = false;
            sKeyWasDown = true; // don't mute right away with the same press
            SPDLOG_INFO("[Coop] voice: mute key {}", key);
            CoopGame::Notify(CoopGame::NoticeKind::Other, "Mute key: " + MuteKeyName());
            return;
        }
        return;
    }
    const bool down = ImGui::IsKeyDown(MuteKey());
    // not while typing in a text box (chat, address...)
    if (Enabled() && down && !sKeyWasDown && !ImGui::GetIO().WantTextInput) {
        CVarSetInteger(VOICE_CVAR("Muted"), MicMuted() ? 0 : 1);
        SaveSettings();
    }
    sKeyWasDown = down;
}

void BeginKeyCapture() {
    sKeyCapture = true;
    sKeyWasDown = false;
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart; ++key) {
        sHeldAtCapture[key - ImGuiKey_NamedKey_BEGIN] = ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
    }
}

// CoC shows "Mic on" / "Mic off" for a second when it changes; we use the co-op pop-up
void WatchMuteChange() {
    const bool now = MicMuted();
    if (!sMuteKnown) {
        sMuteKnown = true;
        sMuteLast = now;
        return;
    }
    if (now == sMuteLast) {
        return;
    }
    sMuteLast = now;
    if (!Enabled()) {
        return;
    }
    CoopGame::Notify(CoopGame::NoticeKind::Other, now ? "Mic off" : "Mic on");
}

void ForgetPlayer(uint32_t id) {
    if (sVoice) {
        sVoice->Forget(id);
    }
    sSpeakers.erase(id);
}

// Every game frame, even with no session, so the mic and speakers close when we leave.
// (CoC voice_update)
void VoiceUpdate() {
    std::lock_guard lock(CoopGame::Mutex());
    UpdateMuteKey();
    WatchMuteChange();
    const bool on = Live();
    if (!sVoice) {
        if (!on) {
            sStatus = Enabled() ? "On, waiting for a co-op session" : "Off";
            return; // never turned on: don't even start SDL audio
        }
        sVoice = std::make_unique<Device>();
    }
    sVoice->Set(on, !MicMuted(), CaptureDevice(), MicVolume() / 100.0f, PlayerVolume() / 100.0f);
    if (!on) {
        sStatus = Enabled() ? "On, waiting for a co-op session" : "Off";
        sPreroll.clear();
        sSpeakers.clear();
        sTalkingUntil = sLocalSpeakingUntil = {};
        return;
    }
    if (!sVoice->Problem().empty()) {
        sStatus = sVoice->Problem();
        return;
    }
    sStatus = MicMuted() ? "On, microphone muted" : "On";

    // someone left: drop their decoder (CoC voice_peer_left)
    for (uint32_t id : sVoice->Voices()) {
        if (CoopGame::Session().FindById(id) == nullptr) {
            ForgetPlayer(id);
        }
    }

    // only send while talking, plus the 2 frames just before and 400 ms after
    const Clock::time_point now = Clock::now();
    for (Frame& frame : sVoice->Take()) {
        if (frame.loud) {
            sLocalSpeakingUntil = now + kSpeakingShown;
            if (sTalkingUntil < now) {
                for (const Frame& early : sPreroll) {
                    SendFrame(early);
                }
            }
            sTalkingUntil = now + kHangover;
            sPreroll.clear();
        }
        if (sTalkingUntil >= now) {
            SendFrame(frame);
        } else {
            sPreroll.push_back(std::move(frame));
            while (static_cast<int>(sPreroll.size()) > kPreroll) {
                sPreroll.pop_front();
            }
        }
    }
}

// A teammate's frame (CoC voice_on_frame + the checks of handle_voice_datagram)
void OnVoice(const Coop::Event& e) {
    if (!sVoice || !Live() || e.origin == 0 || e.origin == CoopGame::SelfId()) {
        return;
    }
    const Coop::RemotePlayer* player = CoopGame::Session().FindById(e.origin);
    if (player == nullptr) {
        return;
    }
    if (CoopGame::IsBlocked(e.origin)) {
        return; // blocked in Co-op > Block players
    }

    Coop::ByteReader r(e.data);
    uint32_t seq = 0;
    uint16_t size = 0;
    if (!r.U32(seq) || !r.U16(size)) {
        return;
    }
    if (seq == 0 || size == 0 || size > kVoiceMaxOpus || r.Remaining() != size) {
        return;
    }
    const uint8_t* opus = e.data.data() + (e.data.size() - size);

    const float gain = GainFor(*player);
    const Clock::time_point now = Clock::now();
    Speaker& speaker = sSpeakers[e.origin];
    if (gain <= 0.01f) {
        speaker.shownUntil = {};
        return;
    }
    // already 8 people talking: newcomers wait
    if (speaker.heardUntil < now) {
        int heard = 0;
        for (const auto& [other, s] : sSpeakers) {
            if (other != e.origin && s.heardUntil >= now) {
                ++heard;
            }
        }
        if (heard >= kMaxHeard) {
            return;
        }
    }
    speaker.heardUntil = now + kHeardFor;
    speaker.shownUntil = now + kSpeakingShown;
    sVoice->Play(e.origin, seq, opus, size, gain, PanFor(*player));
}

// CoC lists the mics again every ~120 frames; here every 4 s while the menu is open
const std::vector<std::string>& InputDevices() {
    if (sDevices.empty() || Clock::now() - sDevicesAt > std::chrono::seconds(4)) {
        sDevicesAt = Clock::now();
        sDevices = sVoice ? sVoice->Microphones() : Device().Microphones();
    }
    return sDevices;
}

// Menu texts from CoC menu.cpp group_global_voice. Only the switch shows while voice is off;
// the rarely touched settings are folded away.
void DrawVoiceMenu() {
    ImGui::SeparatorText("Voice");
    UIWidgets::CVarCheckbox("Voice chat", VOICE_CVAR("Enabled"),
                            UIWidgets::CheckboxOptions().Color(THEME_COLOR).Tooltip("Talk to the players near you."));
    if (!Enabled()) {
        return;
    }
    ImGui::Text("Status: %s", sStatus.c_str());
    std::string talking;
    for (const auto& [id, s] : sSpeakers) {
        if (s.shownUntil > Clock::now()) {
            talking += (talking.empty() ? "" : ", ") + CoopGame::PlayerName(id);
        }
    }
    if (sLocalSpeakingUntil > Clock::now() && !MicMuted()) {
        talking = talking.empty() ? "you" : "you, " + talking;
    }
    if (!talking.empty()) {
        ImGui::Text("Talking: %s", talking.c_str());
    }

    // Microphone list (CoC keeps an index + a name, we just save the name)
    const std::vector<std::string>& mics = InputDevices();
    std::string current = CVarGetString(VOICE_CVAR("Microphone"), kDefaultMic);
    int32_t index = 0;
    for (size_t i = 0; i < mics.size(); ++i) {
        if (mics[i] == current) {
            index = static_cast<int32_t>(i);
        }
    }
    if (UIWidgets::Combobox<int32_t>("Microphone", &index, mics,
                                     UIWidgets::ComboboxOptions()
                                         .LabelPosition(UIWidgets::LabelPositions::Near)
                                         .Color(THEME_COLOR))) {
        if (index >= 0 && static_cast<size_t>(index) < mics.size()) {
            CVarSetString(VOICE_CVAR("Microphone"), mics[index].c_str());
            SaveSettings();
            SPDLOG_INFO("[Coop] voice: microphone '{}'", mics[index]);
        }
    }
    UIWidgets::CVarCheckbox("Mute my microphone", VOICE_CVAR("Muted"),
                            UIWidgets::CheckboxOptions().Color(THEME_COLOR));
    ImGui::TextDisabled("Mute key: %s", sKeyCapture ? "press a key (Esc to cancel)" : MuteKeyName().c_str());

    if (ImGui::TreeNode("More voice settings")) {
        UIWidgets::CVarSliderInt("Microphone volume", VOICE_CVAR("MicVolume"),
                                 UIWidgets::IntSliderOptions()
                                     .Min(0)
                                     .Max(200)
                                     .Step(10)
                                     .DefaultValue(100)
                                     .Format("%d%%")
                                     .Color(THEME_COLOR));
        if (UIWidgets::Button("Change mute key",
                              UIWidgets::ButtonOptions().Color(THEME_COLOR).Tooltip(
                                  "Mutes and unmutes your microphone in game. Press this, then the new key."))) {
            BeginKeyCapture();
        }
        UIWidgets::CVarSliderInt("Player volume", VOICE_CVAR("PlayerVolume"),
                                 UIWidgets::IntSliderOptions()
                                     .Min(0)
                                     .Max(200)
                                     .Step(10)
                                     .DefaultValue(100)
                                     .Format("%d%%")
                                     .Color(THEME_COLOR));
        UIWidgets::CVarSliderInt("Range", VOICE_CVAR("Range"),
                                 UIWidgets::IntSliderOptions()
                                     .Min(0)
                                     .Max(200)
                                     .Step(10)
                                     .DefaultValue(50)
                                     .Format("%d%%")
                                     .Color(THEME_COLOR)
                                     .Tooltip("How far away you can hear people."));
        UIWidgets::CVarCheckbox("Fade with distance in co-op", VOICE_CVAR("Proximity"),
                                UIWidgets::CheckboxOptions().DefaultValue(true).Color(THEME_COLOR).Tooltip(
                                    "Off: everyone in your co-op room is heard at full volume."));
        ImGui::TreePop();
    }
}

void Register() {
    COND_HOOK(OnGameFrameUpdate, true, VoiceUpdate);
    CoopGame::OnEvent(Coop::EventKind::Voice, "CoopVoice", OnVoice);
    CoopGame::AddMenuSection("CoopVoice", CoopGame::MenuWhere::Always, DrawVoiceMenu);
}

static RegisterShipInitFunc sInit(Register);

} // namespace

// For the name tags (CoC puppet.cpp draws a little speaker icon next to the name while they
// talk): true for ~0.5 s after we last played a frame from them. Game thread, session locked.
bool CoopGame::VoiceIsTalking(uint32_t playerId) {
    auto it = sSpeakers.find(playerId);
    return Enabled() && it != sSpeakers.end() && it->second.shownUntil > Clock::now();
}
