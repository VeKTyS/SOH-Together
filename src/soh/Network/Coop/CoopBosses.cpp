// Boss sync: boss adapters, copied from TrueCoop BossAdapters/ (ActorSyncAdapter.h,
// AdapterRegistry.cpp, GohmaAdapter.cpp, EnGomaAdapter.cpp).
//
// A plain enemy is copied as is by CoopEnemies.cpp (host runs it, clients copy
// position, skeleton and the rest of its memory). Bosses are trickier: their
// intro and death are cutscenes that must run on every game. An adapter tells
// CoopEnemies.cpp, per boss, in which "phase" the boss is, when to copy it and
// what to do when the phase changes. Bosses without an adapter are not copied:
// everyone runs their own and only the health is shared.
//
// TrueCoop sends the adapter's extra fields as JSON. We already copy the whole
// enemy memory every frame, so the extras are just "these fields, always copy
// them" (ApplyExtras) and "these fields, never copy them" (no-copy ranges).
// The phase travels in the top 5 bits of EnemyState::submitted.
//
// Not ported: TrueCoop's EnDekubabaAdapter. It needs helper functions TrueCoop
// added to z_en_dekubaba.c (EnDekubaba_MirrorGetStemMode...), which 9.3.1 doesn't
// have. Our memory copy already carries what it streams (actionFunc, stem
// angles, timer), and deaths go through the normal kill messages.

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
#include "src/overlays/actors/ovl_En_Goma/z_en_goma.h"
extern PlayState* gPlayState;
// not static in 9.3.1 (z_boss_goma.c:399)
void BossGoma_SetupDefeated(BossGoma* thisx, PlayState* play);
}

namespace {

// a field inside the actor: offset from the actor start + size
struct Field {
    size_t offset;
    size_t size;
};
#define FIELD(Type, member) Field{ offsetof(Type, member), sizeof(((Type*)nullptr)->member) }

// TrueCoop ActorSyncAdapter. Every part is optional.
struct BossAdapter {
    // host: small phase number from the actor's fields (never from actionFunc)
    uint8_t (*GetPhase)(Actor* actor) = nullptr;
    // client: the host's phase changed (last host pose already applied). True =
    // stop copying it for good, our own game runs it from here (defeat cutscene)
    bool (*OnPhaseChange)(Actor* actor, uint8_t fromPhase, uint8_t toPhase) = nullptr;
    // client: false = run it ourselves this frame (intro cutscene...)
    bool (*ShouldMirror)(Actor* actor, uint8_t streamedPhase) = nullptr;
    // TrueCoop's SerializeExtras / DeserializeExtras fields: always copied from the host
    std::vector<Field> extras;
    // never copied from the host (things the local cutscene owns)
    std::vector<Field> noCopy;
};

// --- Gohma (TrueCoop GohmaAdapter.cpp) ---
//
// The intro cutscene IS Gohma's update, so every game runs its own intro
// (while disableGameplayLogic is set, Gohma has no colliders and no gameplay).
// Copying only starts in FIGHT, once our own intro is over too. On FIGHT ->
// DEFEATED we call BossGoma_SetupDefeated ourselves (just un-freezing it would
// leave a stale action) and our own game plays the death: cutscene, heart
// container, blue warp, clear flag, all local (flags are synced anyway).

enum GohmaPhase : uint8_t {
    GOHMA_PHASE_PREFIGHT = 0,
    GOHMA_PHASE_FIGHT = 1,
    GOHMA_PHASE_DEFEATED = 2,
};

uint8_t Gohma_GetPhase(Actor* actor) {
    BossGoma* goma = (BossGoma*)actor;
    if (goma->disableGameplayLogic) {
        return ((int8_t)actor->colChkInfo.health <= 0) ? GOHMA_PHASE_DEFEATED : GOHMA_PHASE_PREFIGHT;
    }
    return GOHMA_PHASE_FIGHT;
}

bool Gohma_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    SPDLOG_INFO("[Coop] Gohma phase {} -> {}", fromPhase, toPhase);
    if (toPhase == GOHMA_PHASE_DEFEATED && fromPhase == GOHMA_PHASE_FIGHT && gPlayState != nullptr) {
        // last host pose is already applied: play the defeat here from it
        actor->colChkInfo.health = 0;
        BossGoma_SetupDefeated((BossGoma*)actor, gPlayState);
        SPDLOG_INFO("[Coop] Gohma beaten on the host, playing the defeat here");
        return true;
    }
    return false;
}

bool Gohma_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossGoma* g = (BossGoma*)actor;
    // only during the fight proper, and only once our own intro is done
    return streamedPhase == GOHMA_PHASE_FIGHT && !g->disableGameplayLogic;
}

BossAdapter GohmaAdapter() {
    BossAdapter a;
    a.GetPhase = Gohma_GetPhase;
    a.OnPhaseChange = Gohma_OnPhaseChange;
    a.ShouldMirror = Gohma_ShouldMirror;
    // same list as TrueCoop's Gohma_SerializeExtras
    a.extras = {
        FIELD(BossGoma, eyeState),         FIELD(BossGoma, eyeIrisRotX),     FIELD(BossGoma, eyeIrisRotY),
        FIELD(BossGoma, eyeLidBottomRotX), FIELD(BossGoma, eyeLidTopRotX),   FIELD(BossGoma, eyeIrisScaleX),
        FIELD(BossGoma, eyeIrisScaleY),    FIELD(BossGoma, visualState),     FIELD(BossGoma, invincibilityFrames),
        FIELD(BossGoma, mainEnvColor),     FIELD(BossGoma, eyeEnvColor),     FIELD(BossGoma, tailLimbsScale),
    };
    // TrueCoop: "deadLimbsState / decayingProgress / noBackfaceCulling deliberately
    // not streamed: the defeat runs locally on both, and deadLimbsState would
    // double-spawn the limb pieces". Plus the cutscene camera, ours to keep.
    a.noCopy = {
        FIELD(BossGoma, deadLimbsState),   FIELD(BossGoma, decayingProgress),      FIELD(BossGoma, noBackfaceCulling),
        FIELD(BossGoma, subCameraId),      FIELD(BossGoma, subCameraEye),          FIELD(BossGoma, subCameraAt),
        FIELD(BossGoma, subCameraFollowSpeed), FIELD(BossGoma, defeatedLimbPositions),
    };
    return a;
}

// --- Gohma larvas (TrueCoop EnGomaAdapter.cpp) ---
//
// Movement comes from the normal enemy copy. These are the fields its update
// computes for drawing: without gomaType a client would draw a larva as an egg
// forever (the egg -> hatch switch happens in the update, which is frozen).
// Shell pieces and pieces of the dead Gohma (params >= 6) are never copied.

BossAdapter EnGomaAdapter() {
    BossAdapter a;
    // same list as TrueCoop's EnGoma_SerializeExtras
    a.extras = {
        FIELD(EnGoma, gomaType),   FIELD(EnGoma, eggScale),        FIELD(EnGoma, eggPitch),
        FIELD(EnGoma, eggSquishAngle), FIELD(EnGoma, eggSquishAmount), FIELD(EnGoma, eggYOffset),
        FIELD(EnGoma, eyePitch),   FIELD(EnGoma, eyeYaw),          FIELD(EnGoma, hurtTimer),
        FIELD(EnGoma, visualState), FIELD(EnGoma, slopePitch),     FIELD(EnGoma, slopeRoll),
        FIELD(EnGoma, eyeEnvColor),
    };
    a.noCopy = { FIELD(EnGoma, bossLimbDL) }; // a display list pointer
    return a;
}

// TrueCoop AdapterRegistry.cpp. Built on first use, no init order worries.
const std::unordered_map<int16_t, BossAdapter>& Adapters() {
    static const std::unordered_map<int16_t, BossAdapter> adapters = {
        { ACTOR_BOSS_GOMA, GohmaAdapter() },
        { ACTOR_EN_GOMA, EnGomaAdapter() },
    };
    return adapters;
}

const BossAdapter* Find(int16_t actorId) {
    auto it = Adapters().find(actorId);
    return it != Adapters().end() ? &it->second : nullptr;
}

void Register() {
    Adapters(); // build the table now rather than in the middle of a fight
}

} // namespace

// Used by CoopEnemies.cpp (declared there).
namespace CoopBosses {

bool HasAdapter(int16_t actorId) {
    return Find(actorId) != nullptr;
}

uint8_t GetPhase(Actor* actor) {
    const BossAdapter* a = Find(actor->id);
    return (a != nullptr && a->GetPhase != nullptr) ? a->GetPhase(actor) : 0;
}

bool OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    const BossAdapter* a = Find(actor->id);
    return a != nullptr && a->OnPhaseChange != nullptr && a->OnPhaseChange(actor, fromPhase, toPhase);
}

bool ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    const BossAdapter* a = Find(actor->id);
    return a == nullptr || a->ShouldMirror == nullptr || a->ShouldMirror(actor, streamedPhase);
}

// `body` is the host's memory after the common Actor part (EnemyState::body).
void ApplyExtras(Actor* actor, const std::vector<uint8_t>& body) {
    const BossAdapter* a = Find(actor->id);
    if (a == nullptr) {
        return;
    }
    uint8_t* bytes = reinterpret_cast<uint8_t*>(actor);
    for (const Field& f : a->extras) {
        if (f.offset < sizeof(Actor) || f.offset - sizeof(Actor) + f.size > body.size()) {
            continue; // not the same layout (or no body this frame): leave ours
        }
        memcpy(bytes + f.offset, body.data() + (f.offset - sizeof(Actor)), f.size);
    }
}

void AddNoCopyRanges(Actor* actor, std::vector<std::pair<size_t, size_t>>& out) {
    const BossAdapter* a = Find(actor->id);
    if (a == nullptr) {
        return;
    }
    for (const Field& f : a->noCopy) {
        out.emplace_back(f.offset, f.offset + f.size);
    }
}

} // namespace CoopBosses

static RegisterShipInitFunc sInit(Register);
