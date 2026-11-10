// Joining, from Crests of Courage (CoC):
// - join_sync.cpp write_backup / restore_backup / list_backups: before joining
//   someone, our save files are copied aside, and the menu can put the latest
//   or the oldest copy back ("Your save" in CoC's menu.cpp group_advanced).
//   CoC copies the save in memory (its game lets it merge it back while
//   playing). SoH keeps one file per slot on disk (Save/file1.sav...,
//   SaveManager.cpp GetFileName), so we copy those files, and only put them
//   back from the title / file select screen, when no file is loaded.
// - join_sync.cpp keepOwn: "Keep my own rupees, ammo and hearts" decides what
//   CoopItems.cpp ApplySaveBlob keeps when the host's save comes in.
// - mod.cpp begin_migration / migration_update: "Keep playing if the host
//   leaves". CoC hands the room code to the next player. We have no room
//   server, so each player tells the others where they can be reached (their
//   local addresses + port, EventKind::SaveBackup), and when the host is gone
//   the player with the smallest id hosts again and the others join them.
#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/SaveManager.h"
#include "soh/ShipInit.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// libultra's r4300.h has a U32(x) macro (pulled in by the headers above), it breaks ByteReader::U32
#undef U32

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// ============================================================================
// Save backups (CoC join_sync.cpp)
// ============================================================================

const std::string kBackupPrefix = "before_join_";
// our own extra: what was there right before a restore, so a restore never
// throws a save away for good
const std::string kRestorePrefix = "before_restore_";
// CoC kBackupsKept: the 5 newest, plus the very first one (see Prune)
constexpr size_t kBackupsKept = 5;

// CoC s_carryingJoinedWorld: once we took a host's world, the next join must
// not back that up over our real saves. Cleared by a restore.
bool sCarryingJoinedWorld = false;

// The summary line in the menu, read from disk at most every 2 s (the menu
// is drawn every frame)
std::string sSummary;
Clock::time_point sSummaryAt;
bool sSummaryDirty = true;

// Same folder SaveManager uses (Ship::Context::GetPathRelativeToAppDirectory("Save"))
fs::path SaveDir() {
    return fs::path(Ship::Context::GetPathRelativeToAppDirectory("Save"));
}

// Same name as SaveManager::GetFileName (private there): file1.sav ... file3.sav
std::string SaveFileName(int slot) {
    return "file" + std::to_string(slot + 1) + ".sav";
}

// Next to the Save folder, not inside: nothing in SoH ever looks in there
fs::path BackupRoot() {
    return fs::path(Ship::Context::GetPathRelativeToAppDirectory("coop_save_backups"));
}

// for logs (CoC path_text)
std::string PathText(const fs::path& p) {
    std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

// before_join_YYYYMMDD_HHMMSS folders, newest first (CoC list_backups: the
// name sorts like the date)
std::vector<fs::path> ListBackups(const std::string& prefix) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(BackupRoot(), ec)) {
        return out;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(BackupRoot(), ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        std::string name = PathText(entry.path().filename());
        if (name.rfind(prefix, 0) == 0) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end(),
              [](const fs::path& a, const fs::path& b) { return PathText(a.filename()) > PathText(b.filename()); });
    return out;
}

// The save slots (0-2) a backup folder has a copy of
std::vector<int> SlotsIn(const fs::path& dir) {
    std::vector<int> slots;
    std::error_code ec;
    for (int slot = 0; slot < SaveManager::MaxFiles; slot++) {
        if (fs::is_regular_file(dir / SaveFileName(slot), ec)) {
            slots.push_back(slot);
        }
    }
    return slots;
}

std::string SlotList(const std::vector<int>& slots) {
    std::string out;
    for (int slot : slots) {
        out += (out.empty() ? "" : ", ") + std::to_string(slot + 1);
    }
    return out;
}

// "before_join_20261110_140301" -> "2026-11-10 14:03"
std::string WhenOf(const fs::path& dir, const std::string& prefix) {
    std::string stamp = PathText(dir.filename()).substr(prefix.size());
    if (stamp.size() < 13) {
        return "an unknown time";
    }
    return stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2) + " " + stamp.substr(9, 2) + ":" +
           stamp.substr(11, 2);
}

// CoC's rule: keep the kBackupsKept newest AND the oldest one ("the oldest is
// your first one"), delete the ones in between. Only ever touches folders
// ListBackups found inside our own backup folder.
void Prune(const std::string& prefix) {
    std::vector<fs::path> all = ListBackups(prefix);
    for (size_t i = kBackupsKept; i + 1 < all.size(); ++i) {
        std::error_code ec;
        fs::remove_all(all[i], ec);
        if (ec) {
            SPDLOG_WARN("[Coop][Backup] could not delete old backup {}", PathText(all[i]));
        }
    }
}

// CoC write_backup: copies every save slot that exists into a new folder.
// False if there was nothing to copy or it failed (then nothing is left half done).
bool MakeBackup(const std::string& prefix) {
    // a save may still be on its way to the disk (SoH saves on a thread)
    SaveManager::Instance->ThreadPoolWait();

    std::vector<int> slots = SlotsIn(SaveDir());
    if (slots.empty()) {
        SPDLOG_INFO("[Coop][Backup] no save file yet, nothing to back up");
        return false;
    }

    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);

    std::error_code ec;
    fs::create_directories(BackupRoot(), ec);
    if (ec) {
        SPDLOG_WARN("[Coop][Backup] could not create {}, joining without backup", PathText(BackupRoot()));
        return false;
    }
    // two joins in the same second: don't mix them
    fs::path dir = BackupRoot() / (prefix + stamp);
    for (int n = 2; fs::exists(dir, ec); n++) {
        dir = BackupRoot() / (prefix + stamp + "_" + std::to_string(n));
    }
    if (!fs::create_directory(dir, ec) || ec) {
        SPDLOG_WARN("[Coop][Backup] could not create {}, joining without backup", PathText(dir));
        return false;
    }
    for (int slot : slots) {
        fs::copy_file(SaveDir() / SaveFileName(slot), dir / SaveFileName(slot), ec);
        if (ec) {
            SPDLOG_WARN("[Coop][Backup] could not copy {} ({}), backup dropped", SaveFileName(slot), ec.message());
            std::error_code ignored;
            fs::remove_all(dir, ignored); // our own folder from just now, half filled
            return false;
        }
    }
    SPDLOG_INFO("[Coop][Backup] slots {} saved to {}", SlotList(slots), PathText(dir));
    Prune(prefix);
    sSummaryDirty = true;
    return true;
}

// CoC restore_backup. CoC's guard is "disconnected, and a save loaded" because
// it merges into the save in memory. Ours writes the files on disk, so it's
// the other way: disconnected, and NO file loaded (title / file select), or
// the game would write over them at the next save.
void RestoreBackup(bool oldest) {
    if (CoopGame::Session().Role() != Coop::SessionRole::Off) {
        CoopGame::Notify(CoopGame::NoticeKind::Other, "Disconnect first\nYou can only restore while disconnected.");
        return;
    }
    if (gPlayState != nullptr) {
        CoopGame::Notify(CoopGame::NoticeKind::Other, "Not right now\nGo back to the file select screen first.");
        return;
    }
    std::vector<fs::path> all = ListBackups(kBackupPrefix);
    if (all.empty()) {
        CoopGame::Notify(CoopGame::NoticeKind::Other, "No backup found\nOne is made each time you join someone.");
        return;
    }
    const fs::path& chosen = oldest ? all.back() : all.front();
    std::vector<int> slots = SlotsIn(chosen);
    if (slots.empty()) {
        CoopGame::Notify(CoopGame::NoticeKind::Other, "Backup unreadable\nThere's no save file in it.");
        SPDLOG_WARN("[Coop][Backup] {} has no save file", PathText(chosen));
        return;
    }

    // what's there now goes aside first (never lose a save, even to a restore)
    MakeBackup(kRestorePrefix);

    std::error_code ec;
    fs::create_directories(SaveDir(), ec);
    for (int slot : slots) {
        fs::path target = SaveDir() / SaveFileName(slot);
        fs::path temp = target;
        temp += ".restoring";
        // copy next to it, then swap: a failed copy never leaves a broken save
        fs::copy_file(chosen / SaveFileName(slot), temp, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            fs::rename(temp, target, ec);
        }
        if (ec) {
            std::error_code ignored;
            fs::remove(temp, ignored);
            SPDLOG_WARN("[Coop][Backup] could not restore {} ({})", SaveFileName(slot), ec.message());
            CoopGame::Notify(CoopGame::NoticeKind::Other,
                             "Restore failed\nSlot " + std::to_string(slot + 1) + " could not be written.");
            sSummaryDirty = true;
            return;
        }
    }
    // the file select screen reads the slots' names / hearts from SaveManager,
    // read again from disk (the title screen does the same, Sram_InitSram)
    SaveManager::Instance->Init();
    sCarryingJoinedWorld = false;
    sSummaryDirty = true;
    SPDLOG_INFO("[Coop][Backup] restored {}", PathText(chosen));
    CoopGame::Notify(CoopGame::NoticeKind::Other, "Progress restored\nSlot " + SlotList(slots) +
                                                      " is back to how it was on " + WhenOf(chosen, kBackupPrefix) +
                                                      ".");
}

// CoC joinsync_backup_summary
const std::string& BackupSummary() {
    if (!sSummaryDirty && Clock::now() - sSummaryAt < std::chrono::seconds(2)) {
        return sSummary;
    }
    sSummaryDirty = false;
    sSummaryAt = Clock::now();
    std::vector<fs::path> all = ListBackups(kBackupPrefix);
    if (all.empty()) {
        sSummary = "No backups yet.";
        return sSummary;
    }
    std::vector<int> slots = SlotsIn(all.front());
    sSummary = "Latest backup: " + WhenOf(all.front(), kBackupPrefix) +
               (slots.empty() ? std::string() : " (slot " + SlotList(slots) + ")") + ". " + std::to_string(all.size()) +
               " kept. The oldest is your first one.";
    return sSummary;
}

// ============================================================================
// Keep my own rupees, ammo and hearts (CoC join_sync.cpp keepOwn)
// ============================================================================

// CoC s_fileIsNew: a brand-new file takes the host's. SoH has no "new file"
// event, but a file nobody has played yet has firstInput 0 (z_play.c sets it
// at the first button press in game). Decided when the file is loaded, like CoC.
bool sFileIsNew = false;

bool KeepOwnOption() {
    return CVarGetInteger(CVAR_COOP("KeepOwnConsumables"), 1) != 0;
}

// ============================================================================
// Keep playing if the host leaves (CoC mod.cpp begin_migration / migration_update)
// ============================================================================

// sub-type inside EventKind::SaveBackup
constexpr uint8_t kReachMe = 1; // {u16 port, u8 count, count x (u8 len, text)}: where I can be reached
constexpr size_t kMaxAddresses = 8;
constexpr size_t kMaxAddressLength = 15; // "255.255.255.255"
constexpr auto kAnnounceEvery = std::chrono::seconds(5);

// CoC's numbers: successor hosts after 1.5 s, the others try every 4 s, 15 tries
constexpr auto kHostItDelay = std::chrono::milliseconds(1500);
constexpr auto kJoinDelay = std::chrono::milliseconds(4000);
constexpr int kMaxTries = 15;

struct Reach {
    uint16_t port = 0;
    std::vector<std::string> addresses;
};
// what each player told us, by player id (this session's ids)
std::map<uint32_t, Reach> sReach;

struct RosterEntry {
    uint32_t id;
    std::string name;
};
// who was in the session last frame (self included), for when the host is gone
std::vector<RosterEntry> sRoster;
uint32_t sRosterSelf = 0;
Coop::SessionRole sPrevRole = Coop::SessionRole::Off;
Clock::time_point sLastAnnounce;
size_t sAnnouncedPlayers = 0;

// CoC's Migration struct
struct Migration {
    bool active = false;
    bool hostIt = false; // we're the one taking over
    std::string hostName;
    std::vector<std::string> addresses;
    uint16_t port = 0;
    Clock::time_point next;
    int tries = 0;
};
Migration sMigr;

bool KeepPlayingOption() {
    return CVarGetInteger(CVAR_COOP("KeepPlayingIfHostLeaves"), 1) != 0;
}

uint16_t OwnHostPort() {
    return static_cast<uint16_t>(CVarGetInteger(CVAR_COOP("Port"), Coop::kDefaultPort));
}

// "192.168.1.20": 4 numbers 0-255, nothing else (it goes straight into StartJoin)
bool LooksLikeIPv4(const std::string& text) {
    if (text.size() < 7 || text.size() > kMaxAddressLength) {
        return false;
    }
    int parts = 0;
    int digits = 0;
    int value = 0;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (++digits > 3 || value > 255) {
                return false;
            }
        } else if (c == '.') {
            if (digits == 0) {
                return false;
            }
            parts++;
            digits = 0;
            value = 0;
        } else {
            return false;
        }
    }
    return parts == 3 && digits > 0;
}

void AnnounceReach() {
    std::vector<std::string> addresses = Coop::LocalIPv4Addresses();
    Coop::ByteWriter w;
    w.U8(kReachMe);
    w.U16(OwnHostPort());
    std::vector<std::string> sent;
    for (const std::string& a : addresses) {
        if (LooksLikeIPv4(a) && sent.size() < kMaxAddresses) {
            sent.push_back(a);
        }
    }
    w.U8(static_cast<uint8_t>(sent.size()));
    for (const std::string& a : sent) {
        w.U8(static_cast<uint8_t>(a.size()));
        w.Bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(a.data()), a.size()));
    }
    CoopGame::SendEvent(Coop::EventKind::SaveBackup, w.Take());
    sLastAnnounce = Clock::now();
}

void OnReachEvent(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint8_t sub = 0;
    uint16_t port = 0;
    uint8_t count = 0;
    if (!r.U8(sub) || sub != kReachMe || !r.U16(port) || !r.U8(count) || port == 0 || count > kMaxAddresses) {
        return;
    }
    Reach reach;
    reach.port = port;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t len = 0;
        std::string text;
        if (!r.U8(len) || len > kMaxAddressLength || !r.Bytes(len, text) || !LooksLikeIPv4(text)) {
            return; // broken packet
        }
        reach.addresses.push_back(text);
    }
    if (r.Remaining() != 0 || e.origin == 0 || e.origin == CoopGame::SelfId()) {
        return;
    }
    sReach[e.origin] = std::move(reach);
}

// CoC begin_migration: the host is gone. The lowest player id left takes over.
void BeginMigration() {
    if (!KeepPlayingOption() || sRosterSelf == 0 || sRosterSelf == Coop::kHostPlayerId) {
        return;
    }
    const RosterEntry* successor = nullptr;
    for (const RosterEntry& p : sRoster) {
        if (p.id != Coop::kHostPlayerId && (successor == nullptr || p.id < successor->id)) {
            successor = &p;
        }
    }
    if (successor == nullptr) {
        return;
    }
    sMigr = Migration{};
    sMigr.hostIt = successor->id == sRosterSelf;
    if (!sMigr.hostIt) {
        auto it = sReach.find(successor->id);
        if (it == sReach.end() || it->second.addresses.empty()) {
            SPDLOG_INFO("[Coop][Migrate] host gone, successor {} never said where to find them", successor->id);
            CoopGame::Notify(CoopGame::NoticeKind::Players,
                             "The host left\n" + successor->name + " can't take over (their option is off).");
            return;
        }
        sMigr.addresses = it->second.addresses;
        sMigr.port = it->second.port;
        sMigr.hostName = successor->name;
    } else {
        sMigr.port = OwnHostPort();
    }
    sMigr.active = true;
    sMigr.next = Clock::now() + (sMigr.hostIt ? kHostItDelay : kJoinDelay);
    SPDLOG_INFO("[Coop][Migrate] host gone, successor player {}{}", successor->id, sMigr.hostIt ? " (us)" : "");
    if (sMigr.hostIt) {
        CoopGame::Notify(CoopGame::NoticeKind::Players, "The host left\nYou're the host now. Everyone carries on.");
    } else {
        CoopGame::Notify(CoopGame::NoticeKind::Players,
                         "The host left\n" + sMigr.hostName + " is taking over. Rejoining...");
    }
}

// CoC migration_update
void UpdateMigration() {
    if (!sMigr.active) {
        return;
    }
    Coop::SessionRole role = CoopGame::Session().Role();
    if (role == Coop::SessionRole::Connected || role == Coop::SessionRole::Hosting) {
        // back in a session (ours, or one the player started by hand)
        SPDLOG_INFO("[Coop][Migrate] back in the session");
        sMigr = Migration{};
        return;
    }
    if (role == Coop::SessionRole::Connecting || Clock::now() < sMigr.next) {
        return;
    }
    if (++sMigr.tries > kMaxTries) {
        SPDLOG_INFO("[Coop][Migrate] gave up");
        CoopGame::Notify(CoopGame::NoticeKind::Players,
                         sMigr.hostIt
                             ? "Couldn't carry on\nCouldn't host again. Host or join again."
                             : "Couldn't carry on\n" + sMigr.hostName + " didn't come back. Host or join again.");
        sMigr = Migration{};
        return;
    }
    sMigr.next = Clock::now() + kJoinDelay;
    if (sMigr.hostIt) {
        CoopGame::StartHost(sMigr.port);
    } else {
        // their addresses in turn (LAN, Tailscale...), one per try
        const std::string& address = sMigr.addresses[(sMigr.tries - 1) % sMigr.addresses.size()];
        SPDLOG_INFO("[Coop][Migrate] try {} : {}:{}", sMigr.tries, address, sMigr.port);
        CoopGame::StartJoin(address, sMigr.port);
    }
}

// Every game frame, session on or not (our OnEveryFrame callbacks stop with
// the session, and noticing that it stopped is the whole point)
void OnGameFrame() {
    std::lock_guard lock(CoopGame::Mutex());
    Coop::Session& session = CoopGame::Session();
    Coop::SessionRole role = session.Role();

    if (role == Coop::SessionRole::Connected) {
        if (sPrevRole != Coop::SessionRole::Connected) {
            sReach.clear(); // new session, new player ids
            sAnnouncedPlayers = 0;
        }
        sRoster.clear();
        sRosterSelf = session.SelfId();
        sRoster.push_back({ sRosterSelf, "" });
        for (const Coop::RemotePlayer& p : session.Players()) {
            sRoster.push_back({ p.id, p.name });
        }
        // CoC apply_snapshot sets s_carryingJoinedWorld once the host's world is in
        if (CoopGame::FlagsSynced()) {
            sCarryingJoinedWorld = true;
        }
        // tell the others where we are: now, when someone joins, and every few
        // seconds (late joiners). Only if we're up for taking over.
        if (KeepPlayingOption() && sRosterSelf != 0 &&
            (session.Players().size() != sAnnouncedPlayers || Clock::now() - sLastAnnounce > kAnnounceEvery)) {
            sAnnouncedPlayers = session.Players().size();
            AnnounceReach();
        }
    }

    // CoC drop_link on a client: the host went away (crash, "Stop hosting",
    // timeout). Leaving ourselves goes through Session::Stop, which leaves
    // LastError empty: no migration then.
    if (sPrevRole == Coop::SessionRole::Connected && role == Coop::SessionRole::Off && !session.LastError().empty() &&
        !sMigr.active) {
        BeginMigration();
    }
    sPrevRole = role;
    UpdateMigration();
}

// CoC joinsync / write_backup, run by CoopGame::StartJoin before connecting
void BeforeJoin() {
    if (sMigr.active) {
        // CoC: s_joinerApplied = coop_net_migrating(), a rejoin after a host
        // handover keeps our world and makes no backup
        SPDLOG_INFO("[Coop][Backup] rejoining after a host handover, no backup");
        return;
    }
    if (sCarryingJoinedWorld) {
        SPDLOG_INFO("[Coop][Backup] skipped, world is still from an earlier join");
        return;
    }
    if (MakeBackup(kBackupPrefix)) {
        CoopGame::Notify(CoopGame::NoticeKind::Other, "Your saves are backed up\nPut them back from the Co-op menu.");
    }
}

// --- menu (CoC menu.cpp group_joining / group_advanced "Your save" + "Keep playing") ---

bool OptionCheckbox(const char* label, const char* cvar, const char* tooltip) {
    bool value = CVarGetInteger(cvar, 1) != 0;
    if (UIWidgets::Checkbox(label, &value, UIWidgets::CheckboxOptions().Color(THEME_COLOR).Tooltip(tooltip))) {
        CVarSetInteger(cvar, value ? 1 : 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        return true;
    }
    return false;
}

void DrawJoinMenu() {
    if (sMigr.active) {
        // right where they look, with the way out next to it
        if (sMigr.hostIt) {
            ImGui::Text("The host left. You're becoming the host...");
        } else {
            ImGui::Text("The host left. Rejoining %s... (try %d of %d)", sMigr.hostName.c_str(),
                        std::max(sMigr.tries, 1), kMaxTries);
        }
        if (UIWidgets::Button("Stop trying", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Red))) {
            sMigr = Migration{};
        }
    }

    ImGui::SeparatorText("Joining");
    OptionCheckbox("Keep my own ammo and hearts", CVAR_COOP("KeepOwnConsumables"),
                   "Joining with a file you have played: you keep your arrows, bombs, slingshot seeds, sticks, nuts "
                   "and hearts instead of taking the host's, up to what the host's quiver, bags and heart "
                   "containers hold. A brand-new file takes the host's. Either way you never arrive at the host's "
                   "current health - off, or on a new file, you arrive at full.\n"
                   "Rupees are one wallet for everybody here, so you always get the host's.");
    OptionCheckbox("Keep playing if the host leaves", CVAR_COOP("KeepPlayingIfHostLeaves"),
                   "If the host crashes or leaves, another player takes over and everyone carries on.\n"
                   "Works when you can reach each other directly (same network, Tailscale). For this, the other "
                   "players see your local address.");

    ImGui::SeparatorText("Your save");
    ImGui::TextWrapped("%s", BackupSummary().c_str());
    // restoring writes the save files: only with no file loaded
    bool canRestore = gPlayState == nullptr;
    ImGui::BeginDisabled(!canRestore);
    if (UIWidgets::Button("Restore latest backup", UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
        RestoreBackup(false);
    }
    ImGui::SameLine();
    if (UIWidgets::Button("Restore oldest backup", UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
        RestoreBackup(true);
    }
    ImGui::EndDisabled();
    if (!canRestore) {
        ImGui::TextDisabled("Restoring works from the file select screen.");
    }
}

void Register() {
    CoopGame::OnBeforeJoin("CoopJoin", BeforeJoin);
    CoopGame::OnEvent(Coop::EventKind::SaveBackup, "CoopJoin", OnReachEvent);
    CoopGame::AddMenuSection("CoopJoin", CoopGame::MenuWhere::NotConnected, DrawJoinMenu);
    COND_HOOK(OnGameFrameUpdate, true, OnGameFrame);
    // CoC on_file_new / on_file_loaded
    COND_HOOK(OnLoadFile, true, [](int32_t fileNum) {
        sFileIsNew = gSaveContext.ship.stats.firstInput == 0;
        SPDLOG_INFO("[Coop] file {} loaded, {}", fileNum + 1, sFileIsNew ? "brand new" : "already played");
    });
}

} // namespace

namespace CoopGame {

// Read by CoopItems.cpp ApplySaveBlob (CoC load_host_save: keepOwn = !s_fileIsNew && option)
bool JoinKeepsOwnConsumables() {
    return !sFileIsNew && KeepOwnOption();
}

} // namespace CoopGame

static RegisterShipInitFunc sInit(Register);
