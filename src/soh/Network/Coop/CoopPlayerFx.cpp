// Sounds, ocarina, tunic color, join/leave and Ganon messages (Anchor PlayerSfx / OcarinaSfx / tunic VB / GameComplete).
//
// All of it copied from Anchor (soh/Network/Anchor/HookHandlers.cpp + Packets/PlayerSfx.cpp,
// OcarinaSfx.cpp, AllClientState.cpp, GameComplete.cpp). Only the way the data travels changed:
// Anchor sends json packets to the clients in the same scene, we send a small event to everyone
// and the receiver checks the teammate's Link (puppet) is in its scene.

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Notification/Notification.h"
#include "soh/ShipInit.hpp"
#include "soh/ShipUtils.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
extern f32 sRelativeOcarinaVolume; // code_800EC960.c, same extern as Anchor's OcarinaSfx.cpp
}

// libultra's r4300.h has a U32(x) macro (pulled in by the headers above), it breaks ByteWriter::U32
#undef U32

namespace {

// copied from Anchor::IsSaveLoaded (Anchor.cpp)
bool IsSaveLoaded() {
    if (gPlayState == nullptr) {
        return false;
    }
    if (GET_PLAYER(gPlayState) == nullptr) {
        return false;
    }
    if (gSaveContext.fileNum < 0 || gSaveContext.fileNum > 2) {
        return false;
    }
    if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return false;
    }
    return true;
}

// Anchor only sends sounds to "clients in the same scene as the player". We send to everyone
// (the host relays), so we just skip sending when nobody is in our scene. Session locked.
bool AnyoneInMyScene() {
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (p.state && p.state->inGame && p.state->sceneNum == gPlayState->sceneNum) {
            return true;
        }
    }
    return false;
}

// The puppet (their Link in our game) of that player, only if it's standing in our scene.
// Anchor uses clients[clientId].player. Session locked.
Actor* VisiblePuppetOf(uint32_t playerId) {
    for (Actor* puppet : CoopGame::VisiblePuppets()) {
        const Coop::RemotePlayer* owner = CoopGame::PuppetOwner(puppet);
        if (owner != nullptr && owner->id == playerId && owner->state && owner->state->inGame &&
            owner->state->sceneNum == gPlayState->sceneNum) {
            return puppet;
        }
    }
    return nullptr;
}

// Set while we play a teammate's sound, so the OnPlayerSfx hook never sends it back.
// (Player_PlaySfx only fires the hook for ACTOR_PLAYER and puppets aren't, but better safe.)
bool sApplying = false;

// --- 1. player sounds (Anchor Packets/PlayerSfx.cpp) ---

void OnPlayerSfx(u16 sfxId) {
    if (sApplying) {
        return;
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive() || !IsSaveLoaded() || !AnyoneInMyScene()) {
        return;
    }
    Coop::ByteWriter w;
    w.U16(sfxId);
    CoopGame::SendEvent(Coop::EventKind::PlayerSfx, w.Take());
}

void HandlePlayerSfx(const Coop::Event& ev) {
    Coop::ByteReader r(ev.data);
    uint16_t sfxId = 0;
    if (!r.U16(sfxId) || r.Remaining() != 0 || gPlayState == nullptr) {
        return;
    }
    Actor* puppet = VisiblePuppetOf(ev.origin);
    if (puppet == nullptr) {
        return;
    }
    sApplying = true;
    Player_PlaySfx(puppet, sfxId);
    sApplying = false;
}

// --- 2. ocarina notes (Anchor Packets/OcarinaSfx.cpp) ---

// Anchor keeps these in AnchorClient (ocarinaNote / ocarinaModulator / ocarinaBend).
// The audio code keeps a pointer to `modulator` while the note plays, so entries are never
// erased (std::map nodes don't move), the list only grows by one per player id.
struct OcarinaState {
    u8 note = 0xFF; // OCARINA_PITCH_NONE (Anchor's starts at 0, a fresh map entry)
    f32 modulator = 1.0f;
    s8 bend = 0;
};
std::map<uint32_t, OcarinaState> sOcarina;

// Last thing we sent: the hook fires every frame while the ocarina is out, we only send changes.
// The receiver ends up in the exact same state as with Anchor's "every frame".
struct LastOcarinaSent {
    bool valid = false;
    uint8_t note = 0;
    float modulator = 0;
    int8_t bend = 0;
} sLastOcarina;

void OnOcarinaNote(uint8_t note, float modulator, int8_t bend) {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive() || !IsSaveLoaded() || !AnyoneInMyScene()) {
        sLastOcarina.valid = false;
        return;
    }
    if (sLastOcarina.valid && sLastOcarina.note == note && sLastOcarina.modulator == modulator &&
        sLastOcarina.bend == bend) {
        return;
    }
    sLastOcarina = { true, note, modulator, bend };

    uint32_t modBits = 0;
    std::memcpy(&modBits, &modulator, sizeof(modBits));
    Coop::ByteWriter w;
    w.U8(note);
    w.U32(modBits);
    w.U8(static_cast<uint8_t>(bend));
    CoopGame::SendEvent(Coop::EventKind::OcarinaNote, w.Take());
}

void HandleOcarinaNote(const Coop::Event& ev) {
    Coop::ByteReader r(ev.data);
    uint8_t note = 0;
    uint32_t modBits = 0;
    uint8_t bendByte = 0;
    if (!r.U8(note) || !r.U32(modBits) || !r.U8(bendByte) || r.Remaining() != 0 || gPlayState == nullptr) {
        return;
    }
    float modulator = 0;
    std::memcpy(&modulator, &modBits, sizeof(modulator));
    int8_t bend = static_cast<int8_t>(bendByte);
    // garbage check: the game only sends real pitches (C4..Eb5 + flats/sharps) or 0xFF, an
    // instrument id 1..8 (bend - 1 indexes a table in the sequence) and a pitch bend around 1.0
    if ((note > 0x7F && note != 0xFF) || bend < OCARINA_INSTRUMENT_DEFAULT || bend > OCARINA_INSTRUMENT_DEFAULT_COPY2 ||
        !std::isfinite(modulator)) {
        return;
    }
    modulator = CLAMP(modulator, 0.25f, 4.0f);

    Actor* puppet = VisiblePuppetOf(ev.origin);
    if (puppet == nullptr) {
        return;
    }

    // from here on, same as Anchor::HandlePacket_OcarinaSfx
    OcarinaState& client = sOcarina[ev.origin];
    client.modulator = modulator;
    client.bend = bend;

    if ((note != 0xFF) && (client.note != note)) {
        Audio_QueueCmdS8(0x6 << 24 | SEQ_PLAYER_SFX << 16 | 0xD07, client.bend - 1);
        Audio_QueueCmdS8(0x6 << 24 | SEQ_PLAYER_SFX << 16 | 0xD05, note);
        Audio_PlaySfxGeneral(NA_SE_OC_OCARINA, &puppet->projectedPos, 4, &client.modulator, &sRelativeOcarinaVolume,
                             &gSfxDefaultReverb);
    } else if ((client.note != 0xFF) && (note == 0xFF)) {
        Audio_StopSfxById(NA_SE_OC_OCARINA);
    }

    client.note = note;
}

// --- 3. tunic color (Anchor HookHandlers.cpp, VB_APPLY_TUNIC_COLOR) ---

void TintTunic(va_list args) {
    if (gPlayState == nullptr || !CVarGetInteger(CVAR_COOP("TintTunics"), 1)) {
        return;
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive()) {
        return;
    }
    Actor* myPlayer = (Actor*)GET_PLAYER(gPlayState);
    Actor* actor = va_arg(args, Actor*);
    Color_RGB8* color = va_arg(args, Color_RGB8*);

    if (actor == myPlayer) {
        Color_RGB8 ownColor = CoopGame::OwnColor();
        color->r = ownColor.r;
        color->g = ownColor.g;
        color->b = ownColor.b;
        return;
    }

    const Coop::RemotePlayer* client = CoopGame::PuppetOwner(actor);
    if (client == nullptr) {
        return;
    }
    Color_RGB8 theirColor = CoopGame::PlayerColor(*client);
    color->r = theirColor.r;
    color->g = theirColor.g;
    color->b = theirColor.b;
}

// --- 4. join / leave messages (Anchor Packets/AllClientState.cpp) ---

// Who was in the session last frame (id -> name). Anchor compares its client list
// with the new one the server sends; we compare Players() frame to frame.
std::map<uint32_t, std::string> sKnownPlayers;

void CheckJoinLeave() {
    std::map<uint32_t, std::string> now;
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (p.id != 0) {
            now[p.id] = p.name;
        }
    }
    for (const auto& [id, name] : now) {
        if (!sKnownPlayers.contains(id)) {
            Notification::Emit({
                .prefix = name,
                .message = "Connected",
            });
        }
    }
    for (const auto& [id, name] : sKnownPlayers) {
        if (!now.contains(id)) {
            Notification::Emit({
                .prefix = name,
                .message = "Disconnected",
            });
        }
    }
    sKnownPlayers = std::move(now);
}

// --- 4b. Ganon beaten (Anchor Packets/GameComplete.cpp) ---

const std::string gameCompleteMessages[] = {
    "killed Ganon",           "saved Zelda",         "proved their Courage",
    "collected the Triforce", "is the Hero of Time", "proved Mido wrong",
};

void OnGanonDefeated(void* refActor) {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive() || !IsSaveLoaded()) {
        return;
    }
    CoopGame::SendEvent(Coop::EventKind::GameComplete, {});
}

void HandleGameComplete(const Coop::Event& ev) {
    if (!ev.data.empty()) {
        return;
    }
    const Coop::RemotePlayer* client = CoopGame::Session().FindById(ev.origin);
    if (client == nullptr) {
        return;
    }
    Notification::Emit({
        .prefix = client->name,
        .message = ShipUtils::RandomElement(gameCompleteMessages),
    });
}

void Register() {
    // HookHandlers.cpp: COND_HOOK(OnPlayerSfx, ...) / COND_HOOK(OnOcarinaNote, ...)
    COND_HOOK(OnPlayerSfx, true, OnPlayerSfx);
    COND_HOOK(OnOcarinaNote, true, OnOcarinaNote);
    CoopGame::OnEvent(Coop::EventKind::PlayerSfx, "PlayerFx.Sfx", HandlePlayerSfx);
    CoopGame::OnEvent(Coop::EventKind::OcarinaNote, "PlayerFx.Ocarina", HandleOcarinaNote);

    COND_VB_SHOULD(VB_APPLY_TUNIC_COLOR, true, { TintTunic(args); });

    COND_ID_HOOK(OnBossDefeat, ACTOR_BOSS_GANON2, true, OnGanonDefeated);
    CoopGame::OnEvent(Coop::EventKind::GameComplete, "PlayerFx.GameComplete", HandleGameComplete);

    CoopGame::OnEveryFrame("PlayerFx.JoinLeave", CheckJoinLeave);
    // frame callbacks stop when the session ends: forget everyone then, so the next session
    // starts from an empty list (otherwise people with the same ids would never "join")
    COND_HOOK(OnGameFrameUpdate, true, []() {
        std::lock_guard lock(CoopGame::Mutex());
        if (!CoopGame::SessionActive()) {
            sKnownPlayers.clear();
            sLastOcarina.valid = false;
        }
    });
}

} // namespace

static RegisterShipInitFunc sInit(Register);
