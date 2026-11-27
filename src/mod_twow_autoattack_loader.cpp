// SPDX-License-Identifier: GPL-2.0-or-later
// Adaptive attack design based on Auto-Attack-Forever-wotlk.

#include "Chat.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptObjects.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr uint32 AutoShot = 75;
constexpr uint32 WandShoot = 5019;
constexpr uint32 ShootBow = 2480;
constexpr uint32 ShootGun = 7918;
constexpr uint32 ShootCrossbow = 7919;
constexpr uint32 Throw = 2764;
constexpr uint32 RetryMs = 500;
constexpr uint32 TransientRetryMs = 125;
constexpr uint32 TransitionMs = 100;
constexpr uint32 PacedShotAnimMs = 850;
constexpr uint32 ReadyPoseMinRemainingMs = 400;
constexpr size_t ShardCount = 32;
constexpr float MoveResetDistSq = 0.09f; // 0.3 yd movement invalidates geometry backoff

struct PendingShot
{
    ObjectGuid target;
    Spell const* previous;
    uint32 spellId;
    CurrentSpellTypes slot;

    bool operator==(PendingShot const& other) const
    {
        return target == other.target && previous == other.previous &&
            spellId == other.spellId && slot == other.slot;
    }
};

struct Intent
{
    ObjectGuid target;
    Spell const* shot = nullptr;
    uint32 shotId = 0;
    CurrentSpellTypes slot = CURRENT_AUTOREPEAT_SPELL;
    uint32 retry = 0;
    uint32 transition = 0;
    uint32 shotAnim = 0;
    SpellCastResult lastFailure = SPELL_CAST_OK;
    bool ranged = false;
    bool readyEmoteSent = false;
    bool hasFailPos = false;
    float failPlayerX = 0.0f;
    float failPlayerY = 0.0f;
    float failPlayerZ = 0.0f;
    float failTargetX = 0.0f;
    float failTargetY = 0.0f;
    float failTargetZ = 0.0f;
    std::optional<PendingShot> pending;

    bool operator==(Intent const& other) const
    {
        return target == other.target && shot == other.shot && shotId == other.shotId &&
            slot == other.slot && retry == other.retry && transition == other.transition &&
            shotAnim == other.shotAnim && lastFailure == other.lastFailure &&
            ranged == other.ranged && readyEmoteSent == other.readyEmoteSent &&
            hasFailPos == other.hasFailPos &&
            (!hasFailPos || (failPlayerX == other.failPlayerX && failPlayerY == other.failPlayerY &&
                failPlayerZ == other.failPlayerZ && failTargetX == other.failTargetX &&
                failTargetY == other.failTargetY && failTargetZ == other.failTargetZ)) &&
            pending == other.pending;
    }

    bool operator!=(Intent const& other) const { return !(*this == other); }
};

struct Shard
{
    std::mutex mutex;
    std::atomic<uint32> intentCount{0};
    std::unordered_map<uint32, Intent> intents;
    std::unordered_set<uint32> rangedDisabled;
    std::unordered_map<uint32, bool> meleePreferences;
};

std::atomic<bool> enabled{false};
std::atomic<bool> preferencesAvailable{false};
std::atomic<bool> meleePreferencesAvailable{false};
std::atomic<uint32> totalActiveIntents{0};
std::atomic<bool> debug{false};
std::atomic<uint32> debugGuid{0};
std::atomic<bool> rightClick{true};
std::atomic<bool> defaultPreferMelee{true};
std::atomic<bool> announce{false};
std::atomic<bool> stopOnTargetChange{true};
std::array<Shard, ShardCount> shards;

Shard& ShardFor(uint32 guid)
{
    return shards[guid & (ShardCount - 1)];
}

bool HasAnyActiveIntent()
{
    return totalActiveIntents.load(std::memory_order_acquire) != 0;
}

bool ShardHasActiveIntent(Shard const& shard)
{
    return shard.intentCount.load(std::memory_order_acquire) != 0;
}

void UpsertIntentLocked(Shard& shard, uint32 guid, Intent const& intent)
{
    auto [it, inserted] = shard.intents.insert_or_assign(guid, intent);
    (void)it;
    if (inserted)
    {
        shard.intentCount.fetch_add(1, std::memory_order_release);
        totalActiveIntents.fetch_add(1, std::memory_order_release);
    }
}

bool EraseIntentLocked(Shard& shard, uint32 guid, Intent* removed = nullptr)
{
    auto const found = shard.intents.find(guid);
    if (found == shard.intents.end())
        return false;
    if (removed)
        *removed = found->second;
    shard.intents.erase(found);
    shard.intentCount.fetch_sub(1, std::memory_order_release);
    totalActiveIntents.fetch_sub(1, std::memory_order_release);
    return true;
}

bool IsSupportedShotSpell(uint32 spellId)
{
    switch (spellId)
    {
        case AutoShot:
        case WandShoot:
        case ShootBow:
        case ShootGun:
        case ShootCrossbow:
        case Throw:
            return true;
        default:
            return false;
    }
}

template <typename T>
bool PeekPacketValue(WorldPacket const& packet, T& out)
{
    size_t const pos = packet.rpos();
    if (pos > packet.size() || sizeof(T) > packet.size() - pos)
        return false;
    out = packet.read<T>(pos);
    return true;
}

bool EqualsIgnoreCase(char const* lhs, char const* rhs)
{
    if (!lhs || !rhs)
        return false;
    while (*lhs && *rhs)
    {
        if (std::tolower(static_cast<unsigned char>(*lhs)) !=
            std::tolower(static_cast<unsigned char>(*rhs)))
            return false;
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

void EnsureRangedSheath(Player* player, bool forceUpdate = false)
{
    bool const changed = player->GetSheath() != SHEATH_STATE_RANGED;
    if (changed)
        player->SetSheath(SHEATH_STATE_RANGED);
    if (changed || forceUpdate)
        player->DirectSendPublicValueUpdate({UNIT_FIELD_BYTES_2});
}

void EnsureMeleeSheath(Player* player, bool immediate = false)
{
    bool const changed = player->GetSheath() != SHEATH_STATE_MELEE;
    if (changed)
        player->SetSheath(SHEATH_STATE_MELEE);
    if (changed || immediate)
        player->DirectSendPublicValueUpdate({UNIT_FIELD_BYTES_2});
}

uint32 RangedReadyOneShotEmote(Player* player)
{
    Item* weapon = player->GetWeaponForAttack(RANGED_ATTACK, true, true);
    if (!weapon)
        return 0;

    switch (weapon->GetProto()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
            return EMOTE_ONESHOT_READYBOW;
        case ITEM_SUBCLASS_WEAPON_GUN:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return EMOTE_ONESHOT_READYRIFLE;
        default:
            return 0;
    }
}

void Trace(Player* player, ObjectGuid target, char const* event, uint32 spellId = 0, uint32 result = 0)
{
    uint32 const guid = player->GetGUIDLow();
    if (debug && (!debugGuid || debugGuid == guid))
        sLog.outInfo("TwowAutoAttack: player=%u target=%u event=%s spell=%u result=%u",
            guid, target.GetCounter(), event, spellId, result);
}

std::string Lowercase(std::string text)
{
    for (char& ch : text)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

bool PreferMeleeLocked(Shard const& shard, uint32 guid)
{
    auto const found = shard.meleePreferences.find(guid);
    return found == shard.meleePreferences.end() ? defaultPreferMelee.load(std::memory_order_relaxed) : found->second;
}

bool RangedEnabledLocked(Shard const& shard, uint32 guid)
{
    return shard.rangedDisabled.count(guid) == 0;
}

bool PreferMelee(uint32 guid)
{
    Shard& shard = ShardFor(guid);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return PreferMeleeLocked(shard, guid);
}

bool RangedEnabled(uint32 guid)
{
    Shard& shard = ShardFor(guid);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return RangedEnabledLocked(shard, guid);
}

void CacheRangedPreference(uint32 guid, bool ranged)
{
    Shard& shard = ShardFor(guid);
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (ranged)
        shard.rangedDisabled.erase(guid);
    else
        shard.rangedDisabled.insert(guid);
}

bool Eligible(Player* player)
{
    return player && player->IsInWorld() && player->IsAlive() &&
        !player->IsBeingTeleported() && !player->IsMounted() &&
        !player->IsTaxiFlying() && player->IsSelfMover() &&
        !player->GetPossessorGuid() && player->GetSession() &&
        !player->GetSession()->IsHeadless();
}

Spell* OwnedShot(Player* player, Intent const& intent)
{
    Spell* current = player->GetCurrentSpell(intent.slot);
    if (current && current == intent.shot && current->getState() != SPELL_STATE_FINISHED &&
        (intent.slot != CURRENT_GENERIC_SPELL || current->getState() != SPELL_STATE_DELAYED) &&
        current->m_spellInfo->Id == intent.shotId &&
        current->m_targets.getUnitTargetGuid() == intent.target)
        return current;
    return nullptr;
}

Spell* AcceptedShot(Player* player, PendingShot const& pending)
{
    Spell* current = player->GetCurrentSpell(pending.slot);
    if (current && current != pending.previous && current->m_isClientStarted &&
        current->getState() != SPELL_STATE_FINISHED && current->m_spellInfo->Id == pending.spellId &&
        current->m_targets.getUnitTargetGuid() == pending.target)
        return current;
    return nullptr;
}

void StopRangedVictim(Player* player, ObjectGuid target)
{
    Unit* victim = player->GetVictim();
    if (victim && victim->GetObjectGuid() == target && !player->HasUnitState(UNIT_STAT_MELEE_ATTACKING))
        player->AttackStop();
}

void CleanupForgottenIntent(Player* player, Intent const& intent, bool interrupt, char const* reason, bool stopRangedVictim)
{
    Trace(player, intent.target, reason, intent.shotId);
    if (interrupt && player->IsInWorld())
    {
        if (intent.pending && AcceptedShot(player, *intent.pending))
            player->InterruptSpell(intent.pending->slot, false);
        if (OwnedShot(player, intent))
            player->InterruptSpell(intent.slot, false);
    }

    // Only clear a matching ranged engagement. A stock melee swing or a newly
    // chosen server victim belongs to the core.
    if (stopRangedVictim && intent.ranged && player->IsInWorld())
        StopRangedVictim(player, intent.target);
    if (intent.ranged && player->IsInWorld() &&
        player->HasUnitState(UNIT_STAT_MELEE_ATTACKING) &&
        player->GetSheath() == SHEATH_STATE_RANGED)
        EnsureMeleeSheath(player, true);
}

void Forget(Player* player, bool interrupt, char const* reason = "intent cleared", bool stopRangedVictim = false)
{
    if (!HasAnyActiveIntent())
        return;

    uint32 const guid = player->GetGUIDLow();
    Shard& shard = ShardFor(guid);
    if (!ShardHasActiveIntent(shard))
        return;

    Intent intent;
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (!EraseIntentLocked(shard, guid, &intent))
            return;
    }

    CleanupForgottenIntent(player, intent, interrupt, reason, stopRangedVictim);
}

bool ValidTarget(Player* player, Unit* target)
{
    return target && target != player && target->IsInWorld() &&
        !target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING | UNIT_FLAG_NOT_SELECTABLE) &&
        player->IsValidAttackTarget(target);
}

void AdoptPendingShot(Player* player, Intent& intent)
{
    if (!intent.pending)
        return;

    PendingShot const pending = *intent.pending;
    intent.pending.reset();
    Spell* shot = AcceptedShot(player, pending);
    if (!shot)
        return;

    Unit* target = player->GetMap()->GetUnit(pending.target);
    if (!ValidTarget(player, target))
        return;

    if (player->GetVictim() != target)
        player->Attack(target, false);
    if (player->GetVictim() != target)
        return;

    if (Spell* prior = OwnedShot(player, intent))
        if (prior != shot)
            player->InterruptSpell(intent.slot, false);

    intent.target = pending.target;
    intent.shot = shot;
    intent.slot = pending.slot;
    intent.shotId = pending.spellId;
    intent.retry = 0;
    intent.transition = 0;
    intent.lastFailure = SPELL_CAST_OK;
    intent.hasFailPos = false;
    intent.shotAnim = 0;
    intent.readyEmoteSent = false;
    intent.ranged = true;
    EnsureRangedSheath(player);
    Trace(player, intent.target, "native ranged cast adopted", intent.shotId);
}

uint32 WeaponShot(Player* player, uint32 requested = 0)
{
    Item* weapon = player->GetWeaponForAttack(RANGED_ATTACK, true, true);
    if (!weapon)
        return 0;

    uint32 spellId;
    switch (weapon->GetProto()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
            spellId = requested == ShootBow ? ShootBow : AutoShot;
            if (!player->HasActiveSpell(spellId))
                spellId = ShootBow;
            break;
        case ITEM_SUBCLASS_WEAPON_GUN:
            spellId = requested == ShootGun ? ShootGun : AutoShot;
            if (!player->HasActiveSpell(spellId))
                spellId = ShootGun;
            break;
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            spellId = requested == ShootCrossbow ? ShootCrossbow : AutoShot;
            if (!player->HasActiveSpell(spellId))
                spellId = ShootCrossbow;
            break;
        case ITEM_SUBCLASS_WEAPON_THROWN:
            spellId = Throw;
            break;
        case ITEM_SUBCLASS_WEAPON_WAND:
            spellId = WandShoot;
            break;
        default:
            return 0;
    }
    return player->HasActiveSpell(spellId) ? spellId : 0;
}

bool IsPacedShot(uint32 spellId)
{
    return spellId == ShootBow || spellId == ShootGun || spellId == ShootCrossbow || spellId == Throw;
}

uint32 DesiredShot(Player* player, Unit* target, uint32 previousSpell, bool preferMelee)
{
    bool const inMeleeReach = player->CanReachWithMeleeAutoAttack(target);
    if (inMeleeReach && preferMelee)
        return 0;

    uint32 const id = WeaponShot(player, previousSpell);
    if (!id || !inMeleeReach)
        return id;

    SpellEntry const* spell = sSpellMgr.GetSpellEntry(id);
    if (!spell)
        return 0;
    SpellRangeEntry const* range = sSpellRangeStore.LookupEntry(spell->rangeIndex);
    return range && player->GetCombatDistance(target) >= Spells::GetSpellMinRange(range) ? id : 0;
}

SpellCastResult CheckShotGeometry(Player* player, Unit* target, SpellEntry const* info)
{
    // Mirror the strict geometry of the supported, unit-targeted native shots
    // without creating a cast event or consuming a range-modifier charge.
    SpellRangeEntry const* range = sSpellRangeStore.LookupEntry(info->rangeIndex);
    if (!range || info->rangeIndex == SPELL_RANGE_IDX_SELF_ONLY ||
        info->rangeIndex == SPELL_RANGE_IDX_COMBAT)
        return SPELL_CAST_OK;

    float maxRange = Spells::GetSpellMaxRange(range);
    player->ApplySpellMod(info->Id, SPELLMOD_RANGE, maxRange);
    maxRange += 1.25f + player->GetLeewayBonusRange(target, true) +
        player->GetTotalAuraRangeModifier(SPELL_AURA_MOD_ATTACK_AND_SPELL_RANGE) / 1000.0f;
    float const distance = player->GetCombatDistance(target);
    if (distance > maxRange)
        return SPELL_FAILED_OUT_OF_RANGE;
    if (distance < Spells::GetSpellMinRange(range))
        return SPELL_FAILED_TOO_CLOSE;
    if ((info->Custom & SPELL_CUSTOM_FACE_TARGET) && !player->IsFacingTarget(target))
        return SPELL_FAILED_UNIT_NOT_INFRONT;
    if (!(info->AttributesEx2 & SPELL_ATTR_EX2_IGNORE_LOS) && !player->IsWithinLOSInMap(target))
        return SPELL_FAILED_LINE_OF_SIGHT;
    return SPELL_CAST_OK;
}

bool MovedSinceFailure(Player* player, Unit* target, Intent const& intent)
{
    if (intent.lastFailure == SPELL_FAILED_UNIT_NOT_INFRONT && player->IsFacingTarget(target))
        return true;

    float const pdx = player->GetPositionX() - intent.failPlayerX;
    float const pdy = player->GetPositionY() - intent.failPlayerY;
    float const pdz = player->GetPositionZ() - intent.failPlayerZ;
    if (pdx * pdx + pdy * pdy + pdz * pdz > MoveResetDistSq)
        return true;

    float const tdx = target->GetPositionX() - intent.failTargetX;
    float const tdy = target->GetPositionY() - intent.failTargetY;
    float const tdz = target->GetPositionZ() - intent.failTargetZ;
    return tdx * tdx + tdy * tdy + tdz * tdz > MoveResetDistSq;
}

class Settings final : public WorldScript
{
public:
    Settings() : WorldScript("TwowAutoAttackSettings", {WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_CONFIG_LOAD}) {}

    void LoadRuntimeConfig()
    {
        debug.store(sConfig.GetBoolDefault("TwowAutoAttack.Debug", false), std::memory_order_relaxed);
        int32 const filter = sConfig.GetIntDefault("TwowAutoAttack.Debug.PlayerGuid", 0);
        debugGuid.store(filter > 0 ? uint32(filter) : 0, std::memory_order_relaxed);
        rightClick.store(sConfig.GetBoolDefault("TwowAutoAttack.RightClick", true), std::memory_order_relaxed);
        defaultPreferMelee.store(sConfig.GetBoolDefault("TwowAutoAttack.CloseRange.Melee", true), std::memory_order_relaxed);
        announce.store(sConfig.GetBoolDefault("TwowAutoAttack.Announce", false), std::memory_order_relaxed);
        stopOnTargetChange.store(sConfig.GetBoolDefault("TwowAutoAttack.StopOnTargetChange", true), std::memory_order_relaxed);
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LoadRuntimeConfig();
    }

    void OnStartup() override
    {
        LoadRuntimeConfig();
        bool const isEnabled = sConfig.GetBoolDefault("TwowAutoAttack.Enable", false);
        enabled.store(isEnabled, std::memory_order_release);
        sLog.outInfo("TwowAutoAttack: adaptive attacks %s.", isEnabled ? "enabled" : "disabled");
        if (isEnabled)
        {
            bool hasRangedTable = false;
            bool hasMeleeTable = false;
            std::unique_ptr<QueryResult> tables(CharacterDatabase.PQuery(
                "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                "AND TABLE_NAME IN ('mod_twow_autoattack_ranged_disabled', 'mod_twow_autoattack_melee_preference')"));
            if (tables)
            {
                do
                {
                    char const* tableName = tables->Fetch()[0].GetString();
                    if (tableName && std::strcmp(tableName, "mod_twow_autoattack_ranged_disabled") == 0)
                        hasRangedTable = true;
                    else if (tableName && std::strcmp(tableName, "mod_twow_autoattack_melee_preference") == 0)
                        hasMeleeTable = true;
                } while (tables->NextRow());
            }

            preferencesAvailable.store(hasRangedTable, std::memory_order_release);
            meleePreferencesAvailable.store(hasMeleeTable, std::memory_order_release);

            if (hasRangedTable)
            {
                std::unique_ptr<QueryResult> rows(CharacterDatabase.PQuery(
                    "SELECT guid FROM mod_twow_autoattack_ranged_disabled"));
                if (rows)
                {
                    do
                    {
                        uint32 const guid = rows->Fetch()[0].GetUInt32();
                        Shard& shard = ShardFor(guid);
                        std::lock_guard<std::mutex> lock(shard.mutex);
                        shard.rangedDisabled.insert(guid);
                    } while (rows->NextRow());
                }
            }

            if (hasMeleeTable)
            {
                std::unique_ptr<QueryResult> rows(CharacterDatabase.PQuery(
                    "SELECT guid, prefer_melee FROM mod_twow_autoattack_melee_preference"));
                if (rows)
                {
                    do
                    {
                        Field* fields = rows->Fetch();
                        uint32 const guid = fields[0].GetUInt32();
                        bool const prefer = fields[1].GetUInt32() != 0;
                        Shard& shard = ShardFor(guid);
                        std::lock_guard<std::mutex> lock(shard.mutex);
                        shard.meleePreferences[guid] = prefer;
                    } while (rows->NextRow());
                }
            }
        }
    }
};

class RangedCommands final : public AllCommandScript
{
public:
    RangedCommands() : AllCommandScript("TwowAutoAttackCommands") {}

    bool CanExecuteCommand(ChatHandler* handler, char const* command, char const* arguments) override
    {
        if (!handler || !command ||
            (!EqualsIgnoreCase(command, "aaf") && !EqualsIgnoreCase(command, "rangedauto")))
            return true;

        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || session->IsHeadless() || !enabled.load(std::memory_order_relaxed))
        {
            handler->SendSysMessage("Auto-Attack Forever is unavailable.");
            return false;
        }

        std::istringstream input(arguments ? arguments : "");
        std::string section, action, extra;
        uint32 const guid = player->GetGUIDLow();
        input >> section;
        section = Lowercase(section);
        if (section.empty() || (section == "status" && !(input >> extra)))
        {
            bool ranged = true;
            bool preferMelee = defaultPreferMelee;
            {
                Shard& shard = ShardFor(guid);
                std::lock_guard<std::mutex> lock(shard.mutex);
                ranged = RangedEnabledLocked(shard, guid);
                preferMelee = PreferMeleeLocked(shard, guid);
            }
            handler->SendSysMessage(ranged ? "AAF_RANGED 1" : "AAF_RANGED 0");
            handler->SendSysMessage(preferMelee ? "AAF_MELEE 1" : "AAF_MELEE 0");
            return false;
        }

        input >> action;
        action = Lowercase(action);
        if ((section != "ranged" && section != "melee") ||
            (action != "status" && action != "on" && action != "off") || (input >> extra))
        {
            handler->SendSysMessage("Usage: .aaf ranged|melee on|off|status");
            return false;
        }

        if (section == "melee")
        {
            if (action != "status")
            {
                if (!meleePreferencesAvailable.load(std::memory_order_relaxed))
                {
                    handler->SendSysMessage("Auto-Attack Forever melee preferences are unavailable.");
                    return false;
                }
                bool const prefer = action == "on";
                if (!CharacterDatabase.DirectPExecute(
                    "INSERT INTO mod_twow_autoattack_melee_preference (guid, prefer_melee) VALUES (%u, %u) "
                    "ON DUPLICATE KEY UPDATE prefer_melee = VALUES(prefer_melee)", guid, prefer ? 1u : 0u))
                {
                    handler->SendSysMessage("Auto-Attack Forever could not save the melee preference.");
                    return false;
                }
                Shard& shard = ShardFor(guid);
                std::lock_guard<std::mutex> lock(shard.mutex);
                shard.meleePreferences[guid] = prefer;
            }
            handler->SendSysMessage(PreferMelee(guid) ? "AAF_MELEE 1" : "AAF_MELEE 0");
            return false;
        }

        if (action != "status")
        {
            if (!preferencesAvailable.load(std::memory_order_relaxed))
            {
                handler->SendSysMessage("Auto-Attack Forever ranged preferences are unavailable.");
                return false;
            }

            bool const ranged = action == "on";
            bool const saved = ranged
                ? CharacterDatabase.DirectPExecute(
                    "DELETE FROM mod_twow_autoattack_ranged_disabled WHERE guid = %u", guid)
                : CharacterDatabase.DirectPExecute(
                    "INSERT IGNORE INTO mod_twow_autoattack_ranged_disabled (guid) VALUES (%u)", guid);
            if (!saved)
            {
                handler->SendSysMessage("Auto-Attack Forever could not save the ranged preference.");
                return false;
            }
            CacheRangedPreference(guid, ranged);
        }

        handler->SendSysMessage(RangedEnabled(guid) ? "AAF_RANGED 1" : "AAF_RANGED 0");
        return false;
    }
};

class AttackPackets final : public ServerScript
{
public:
    AttackPackets() : ServerScript("TwowAutoAttackPackets", {SERVERHOOK_CAN_PACKET_RECEIVE}) {}

    bool CanPacketReceive(WorldSession* session, WorldPacket const& incoming) override
    {
        if (!enabled.load(std::memory_order_relaxed) || !session)
            return true;

        uint16 const opcode = incoming.GetOpcode();
        switch (opcode)
        {
            case CMSG_ATTACKSWING:
            case CMSG_ATTACKSTOP:
            case CMSG_CANCEL_AUTO_REPEAT_SPELL:
            case CMSG_CAST_SPELL:
            case CMSG_CANCEL_CAST:
            case CMSG_SET_SELECTION:
            case CMSG_SETSHEATHED:
                break;
            default:
                return true;
        }

        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || session->IsHeadless())
            return true;

        uint32 const playerGuid = player->GetGUIDLow();

        if (opcode == CMSG_ATTACKSTOP)
        {
            Forget(player, true, "attack stopped by client");
            return true;
        }

        if (opcode == CMSG_SETSHEATHED)
        {
            if (!HasAnyActiveIntent())
                return true;

            uint32 sheathed = 0;
            if (!PeekPacketValue(incoming, sheathed) || sheathed != SHEATH_STATE_MELEE)
                return true;

            Shard& shard = ShardFor(playerGuid);
            if (!ShardHasActiveIntent(shard))
                return true;

            Intent intent;
            bool rangedEnabled = true;
            bool preferMelee = defaultPreferMelee.load(std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                auto const found = shard.intents.find(playerGuid);
                if (found == shard.intents.end())
                    return true;
                intent = found->second;
                rangedEnabled = RangedEnabledLocked(shard, playerGuid);
                preferMelee = PreferMeleeLocked(shard, playerGuid);
            }

            bool const accepted = intent.pending && AcceptedShot(player, *intent.pending);
            bool const managedRanged = intent.ranged || accepted;
            if (!managedRanged || !rangedEnabled || player->HasUnitState(UNIT_STAT_MELEE_ATTACKING))
                return true;

            ObjectGuid const targetGuid = accepted ? intent.pending->target : intent.target;
            Unit* target = targetGuid.IsUnit() ? player->GetMap()->GetUnit(targetGuid) : nullptr;
            if (!ValidTarget(player, target) || !DesiredShot(player, target, intent.shotId, preferMelee))
                return true;

            // Keep the ranged weapon held between ranged shots; the melee weapon
            // is drawn only once the target enters melee range.
            EnsureRangedSheath(player, true);
            return false;
        }

        if (opcode == CMSG_CANCEL_AUTO_REPEAT_SPELL)
        {
            if (!HasAnyActiveIntent())
                return true;

            Shard& shard = ShardFor(playerGuid);
            if (!ShardHasActiveIntent(shard))
                return true;

            Intent removed;
            bool cancelOwnedRepeat = false;
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                auto const found = shard.intents.find(playerGuid);
                if (found != shard.intents.end())
                {
                    Intent const& intent = found->second;
                    cancelOwnedRepeat = (intent.slot == CURRENT_AUTOREPEAT_SPELL &&
                        (OwnedShot(player, intent) ||
                            (intent.shotId == WandShoot && intent.ranged && !intent.transition && !intent.pending))) ||
                        (intent.pending && intent.pending->slot == CURRENT_AUTOREPEAT_SPELL &&
                            AcceptedShot(player, *intent.pending));
                    if (cancelOwnedRepeat)
                    {
                        removed = intent;
                        shard.intents.erase(found);
                        shard.intentCount.fetch_sub(1, std::memory_order_release);
                        totalActiveIntents.fetch_sub(1, std::memory_order_release);
                    }
                }
            }
            if (cancelOwnedRepeat)
                CleanupForgottenIntent(player, removed, false, "ranged repeat cancelled by client", true);
            // A late cancel with an empty repeat slot cannot stop a new intent
            // or an independently timed Shoot/Throw cast.
            return true;
        }

        if (opcode == CMSG_SET_SELECTION)
        {
            if (!HasAnyActiveIntent())
                return true;

            Shard& shard = ShardFor(playerGuid);
            if (!ShardHasActiveIntent(shard))
                return true;

            uint64 rawSelection = 0;
            if (!PeekPacketValue(incoming, rawSelection))
                return true;
            ObjectGuid const selection(rawSelection);

            Intent intent;
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                auto const found = shard.intents.find(playerGuid);
                if (found == shard.intents.end())
                    return true;
                intent = found->second;
            }

            bool const accepted = intent.pending && AcceptedShot(player, *intent.pending);
            ObjectGuid const managedTarget = accepted ? intent.pending->target : intent.target;
            bool const managedRanged = intent.ranged || accepted;
            if (!managedRanged || selection == managedTarget)
                return true;

            if (stopOnTargetChange && (selection.IsEmpty() || player->GetSelectionGuid() == managedTarget))
            {
                Forget(player, true, "ranged target selection changed", true);
                return true;
            }

            bool const pendingRepeat = accepted && intent.pending->slot == CURRENT_AUTOREPEAT_SPELL;
            bool const managedRepeat = intent.slot == CURRENT_AUTOREPEAT_SPELL && intent.shotId != 0;
            if (!pendingRepeat && !managedRepeat)
                return true;

            Unit* target = selection.IsUnit() ? player->GetMap()->GetUnit(selection) : nullptr;
            if (!ValidTarget(player, target))
                Forget(player, true, "ranged selection cleared or invalid", true);
            else if (!stopOnTargetChange && Eligible(player) && (pendingRepeat || OwnedShot(player, intent)))
            {
                if (player->GetVictim() != target)
                    player->Attack(target, false);
                if (player->GetVictim() != target)
                    Forget(player, true, "native retarget failed", true);
                else
                {
                    // The stock selection handler updates the spell's target next.
                    if (pendingRepeat)
                        intent.pending->target = selection;
                    else
                        intent.target = selection;
                    std::lock_guard<std::mutex> lock(shard.mutex);
                    UpsertIntentLocked(shard, playerGuid, intent);
                }
            }
            return true;
        }

        if (opcode == CMSG_CANCEL_CAST)
        {
            if (!HasAnyActiveIntent())
                return true;

            Shard& shard = ShardFor(playerGuid);
            if (!ShardHasActiveIntent(shard))
                return true;

            uint32 spellId = 0;
            if (!PeekPacketValue(incoming, spellId))
                return true;

            Intent removed;
            bool cancel = false;
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                auto const found = shard.intents.find(playerGuid);
                if (found != shard.intents.end())
                {
                    Intent const& intent = found->second;
                    cancel = ((!spellId || spellId == intent.shotId) && OwnedShot(player, intent)) ||
                        (intent.pending && (!spellId || spellId == intent.pending->spellId) &&
                            AcceptedShot(player, *intent.pending));
                    if (cancel)
                    {
                        removed = intent;
                        shard.intents.erase(found);
                        shard.intentCount.fetch_sub(1, std::memory_order_release);
                        totalActiveIntents.fetch_sub(1, std::memory_order_release);
                    }
                }
            }
            if (cancel)
                CleanupForgottenIntent(player, removed, false, "ranged cast cancelled by client", true);
            return true;
        }

        if (opcode == CMSG_CAST_SPELL)
        {
            uint32 spellId = 0;
            if (!PeekPacketValue(incoming, spellId) || !IsSupportedShotSpell(spellId))
                return true;

            if (!Eligible(player) || spellId != WeaponShot(player, spellId))
                return true;

            SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
            if (!spell || spell->IsPassiveSpell())
                return true;

            Shard& shard = ShardFor(playerGuid);
            Intent intent;
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                if (!RangedEnabledLocked(shard, playerGuid))
                    return true;
                auto const found = shard.intents.find(playerGuid);
                if (found != shard.intents.end())
                    intent = found->second;
            }

            WorldPacket packet(incoming);
            uint32 skippedSpellId = 0;
            packet >> skippedSpellId;
            SpellCastTargets targets;
            packet >> targets.ReadForCaster(player);
            Unit* target = targets.getUnitTarget();
            if (!ValidTarget(player, target))
                return true;

            CurrentSpellTypes const slot = spell->IsAutoRepeatRangedSpell()
                ? CURRENT_AUTOREPEAT_SPELL : CURRENT_GENERIC_SPELL;
            AdoptPendingShot(player, intent);
            intent.pending = PendingShot{target->GetObjectGuid(), player->GetCurrentSpell(slot), spellId, slot};
            std::lock_guard<std::mutex> lock(shard.mutex);
            UpsertIntentLocked(shard, playerGuid, intent);
            return true;
        }

        // CMSG_ATTACKSWING
        if (!Eligible(player))
            return true;

        uint64 rawGuid = 0;
        if (!PeekPacketValue(incoming, rawGuid))
            return true;
        ObjectGuid const guid(rawGuid);

        Unit* target = guid.IsUnit() ? player->GetMap()->GetUnit(guid) : nullptr;
        if (!ValidTarget(player, target) || player->IsFriendlyTo(target))
            return true;

        if (!rightClick)
        {
            Forget(player, true, "stock attack requested");
            return true;
        }

        Shard& shard = ShardFor(playerGuid);
        bool rangedEnabled = true;
        bool preferMelee = defaultPreferMelee;
        bool hasExisting = false;
        Intent existing;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            rangedEnabled = RangedEnabledLocked(shard, playerGuid);
            preferMelee = PreferMeleeLocked(shard, playerGuid);
            auto const found = shard.intents.find(playerGuid);
            if (found != shard.intents.end())
            {
                hasExisting = true;
                existing = found->second;
            }
        }

        uint32 const directShot = rangedEnabled
            ? DesiredShot(player, target, 0, preferMelee) : 0;
        if (hasExisting)
        {
            bool const samePending = existing.pending && existing.pending->target == guid &&
                AcceptedShot(player, *existing.pending);
            bool const sameActive = existing.target == guid && player->GetVictim() == target;
            if (samePending || sameActive)
            {
                if (directShot)
                {
                    player->SendMeleeAttackStop(target);
                    EnsureRangedSheath(player, true);
                }
                return !directShot;
            }
        }

        Intent intent{guid};
        if (directShot)
        {
            // Establish the native victim without starting the melee loop.
            // A target rejected by Unit::Attack must leave the previous shot intact.
            player->Attack(target, false);
            if (player->GetVictim() != target)
                return true;
            player->SendMeleeAttackStop(target);
            intent.ranged = true;
            EnsureRangedSheath(player, true);
            Trace(player, guid, "ranged-first attack", directShot);
        }
        else if (player->GetSheath() == SHEATH_STATE_RANGED)
            EnsureMeleeSheath(player, true);

        Intent previous;
        bool hadPrevious = false;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto const found = shard.intents.find(playerGuid);
            if (found != shard.intents.end())
            {
                hadPrevious = true;
                previous = found->second;
                found->second = intent;
            }
            else
                UpsertIntentLocked(shard, playerGuid, intent);
        }
        if (hadPrevious)
            CleanupForgottenIntent(player, previous, true, "attack target changed", false);

        // Melee requests still go through the stock swing handler.
        return !directShot;
    }
};

class AttackUpdates final : public PlayerScript
{
public:
    AttackUpdates() : PlayerScript("TwowAutoAttackUpdates",
        {PLAYERHOOK_ON_BEFORE_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_MAP_CHANGED,
            PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_PLAYER_JUST_DIED}) {}

    void OnLogin(Player* player) override
    {
        if (!enabled.load(std::memory_order_relaxed) || !player || !player->GetSession() ||
            player->GetSession()->IsHeadless())
            return;

        if (announce)
            ChatHandler(player).SendSysMessage("Auto-Attack Forever is enabled. Use .aaf status for your settings.");
    }

    void OnLogout(Player* player) override
    {
        if (!enabled.load(std::memory_order_relaxed) || !player || !player->GetSession() ||
            player->GetSession()->IsHeadless())
            return;
        Forget(player, false, "logout");
    }

    void OnMapChanged(Player* player) override
    {
        if (!enabled.load(std::memory_order_relaxed) || !player || !player->GetSession() ||
            player->GetSession()->IsHeadless())
            return;
        Forget(player, false, "map changed");
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!enabled.load(std::memory_order_relaxed) || !player || !player->GetSession() ||
            player->GetSession()->IsHeadless())
            return;
        Forget(player, false, "player died");
    }

    void OnSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!enabled.load(std::memory_order_relaxed) || !HasAnyActiveIntent() ||
            !IsPacedShot(spell->m_spellInfo->Id) || spell->IsAutoRepeat() ||
            spell->IsTriggered() || !player->GetSession() ||
            player->GetSession()->IsHeadless())
            return;

        uint32 const guid = player->GetGUIDLow();
        Shard& shard = ShardFor(guid);
        if (!ShardHasActiveIntent(shard))
            return;

        bool paced = false;
        bool stillRanged = false;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto const found = shard.intents.find(guid);
            if (found != shard.intents.end())
            {
                paced = found->second.slot == CURRENT_GENERIC_SPELL && OwnedShot(player, found->second) == spell;
                stillRanged = found->second.ranged;
                if (paced)
                {
                    found->second.shotAnim = PacedShotAnimMs;
                    found->second.readyEmoteSent = false;
                }
            }
        }
        if (paced)
        {
            player->ResetAttackTimer(RANGED_ATTACK);
            if (stillRanged && !player->HasUnitState(UNIT_STAT_MELEE_ATTACKING))
                EnsureRangedSheath(player);
        }
    }

    void OnBeforeUpdate(Player* player, uint32 diff) override
    {
        if (!enabled.load(std::memory_order_relaxed) || !HasAnyActiveIntent() ||
            !player->GetSession() || player->GetSession()->IsHeadless())
            return;

        uint32 const guid = player->GetGUIDLow();
        Shard& shard = ShardFor(guid);
        if (!ShardHasActiveIntent(shard))
            return;

        Intent intent;
        bool ranged = true;
        bool preferMelee = defaultPreferMelee;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto const found = shard.intents.find(guid);
            if (found == shard.intents.end())
                return;
            intent = found->second;
            ranged = RangedEnabledLocked(shard, guid);
            preferMelee = PreferMeleeLocked(shard, guid);
        }

        Intent const initial = intent;

        if (!Eligible(player))
        {
            Forget(player, true, "player unavailable", true);
            return;
        }

        if (intent.pending)
        {
            AdoptPendingShot(player, intent);
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                auto const found = shard.intents.find(guid);
                if (found == shard.intents.end())
                    return;
                found->second = intent;
            }
        }

        Unit* target = intent.target.IsEmpty() ? nullptr : player->GetMap()->GetUnit(intent.target);
        if (!ValidTarget(player, target))
        {
            Forget(player, true, "target unavailable", true);
            return;
        }

        if (player->GetVictim() != target)
        {
            Forget(player, true);
            return;
        }

        intent.retry = diff >= intent.retry ? 0 : intent.retry - diff;
        intent.transition = diff >= intent.transition ? 0 : intent.transition - diff;
        intent.shotAnim = diff >= intent.shotAnim ? 0 : intent.shotAnim - diff;
        if (intent.retry && intent.hasFailPos && MovedSinceFailure(player, target, intent))
        {
            intent.retry = 0;
            intent.hasFailPos = false;
        }

        Spell* current = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        Spell* owned = OwnedShot(player, intent);
        if (current && current != owned)
        {
            Forget(player, false);
            return;
        }

        uint32 const desired = ranged ? DesiredShot(player, target, intent.shotId, preferMelee) : 0;
        if (owned && desired != intent.shotId)
        {
            player->InterruptSpell(intent.slot, false);
            owned = nullptr;
            if (intent.slot == CURRENT_AUTOREPEAT_SPELL)
                intent.transition = TransitionMs;
        }
        intent.shot = owned;

        if (!desired)
        {
            if (intent.ranged)
            {
                EnsureMeleeSheath(player, true);
                player->Attack(target, true);
                Trace(player, intent.target, "switched to melee");
            }
            else if (player->HasUnitState(UNIT_STAT_MELEE_ATTACKING) &&
                player->GetSheath() == SHEATH_STATE_RANGED)
                EnsureMeleeSheath(player, true);
            intent.ranged = false;
            intent.retry = 0;
            intent.shotAnim = 0;
            intent.readyEmoteSent = false;
            intent.lastFailure = SPELL_CAST_OK;
            intent.hasFailPos = false;
        }
        else
        {
            bool const wasRanged = intent.ranged;
            intent.ranged = true;
            if (player->HasUnitState(UNIT_STAT_MELEE_ATTACKING))
            {
                player->ClearUnitState(UNIT_STAT_MELEE_ATTACKING);
                player->SendMeleeAttackStop(target);
                EnsureRangedSheath(player, true);
                Trace(player, intent.target, "switched to ranged", desired);
            }
            else
                EnsureRangedSheath(player, !wasRanged);

            // Auto Shot can be armed while moving. The core pauses it until
            // stationary, while wand Shoot would be cancelled each update.
            if (!owned && !intent.retry && !intent.transition &&
                (!player->IsMoving() || desired == AutoShot) &&
                !player->IsNonMeleeSpellCasted(false, false, true))
            {
                SpellEntry const* info = sSpellMgr.GetSpellEntry(desired);
                // Native repeats enforce the weapon timer when firing. Single casts need
                // the same timer check here because each shot is a separate spell.
                if (info && (info->IsAutoRepeatRangedSpell() || player->IsAttackReady(RANGED_ATTACK)) &&
                    !player->HasSpellCooldown(info->Id) &&
                    !player->HasSpellCategoryCooldown(info->Category) &&
                    !player->GetGlobalCooldownMgr().HasGlobalCooldown(info))
                {
                    SpellCastResult const geometry = CheckShotGeometry(player, target, info);
                    if (geometry != SPELL_CAST_OK)
                    {
                        if (geometry != intent.lastFailure)
                            Trace(player, intent.target, "ranged shot waiting", desired, uint32(geometry));
                        intent.retry = geometry == intent.lastFailure ? RetryMs : TransientRetryMs;
                        intent.lastFailure = geometry;
                        intent.hasFailPos = true;
                        intent.failPlayerX = player->GetPositionX();
                        intent.failPlayerY = player->GetPositionY();
                        intent.failPlayerZ = player->GetPositionZ();
                        intent.failTargetX = target->GetPositionX();
                        intent.failTargetY = target->GetPositionY();
                        intent.failTargetZ = target->GetPositionZ();
                    }
                    else
                    {
                        intent.hasFailPos = false;
                        CurrentSpellTypes const slot = info->IsAutoRepeatRangedSpell()
                            ? CURRENT_AUTOREPEAT_SPELL : CURRENT_GENERIC_SPELL;
                        SpellCastTargets targets;
                        targets.setUnitTarget(target);
                        Spell* spell = new Spell(player, info, false, ObjectGuid(), nullptr, target);
                        SpellCastResult const result = spell->prepare(targets);
                        Trace(player, intent.target, "ranged cast attempt", desired, uint32(result));
                        if (result == SPELL_CAST_OK)
                        {
                            if (player->GetCurrentSpell(slot) == spell)
                            {
                                owned = spell;
                                intent.shot = spell;
                                intent.shotId = desired;
                                intent.slot = slot;
                            }
                        }
                        else if (result == SPELL_FAILED_NO_AMMO || result == SPELL_FAILED_NO_CHARGES_REMAIN)
                        {
                            StopRangedVictim(player, intent.target);
                            Forget(player, false, "ranged attack ran out of ammunition");
                            return;
                        }
                        else
                            intent.retry = RetryMs;
                        intent.lastFailure = result;
                    }
                }
            }

            if (IsPacedShot(desired) && !owned && !intent.shotAnim && !intent.readyEmoteSent &&
                player->GetSheath() == SHEATH_STATE_RANGED &&
                !player->IsMoving() &&
                !player->IsNonMeleeSpellCasted(false, false, true) &&
                player->GetStandState() == UNIT_STAND_STATE_STAND &&
                !player->HasUnitState(UNIT_STAT_CAN_NOT_REACT) &&
                player->GetAttackTimer(RANGED_ATTACK) > ReadyPoseMinRemainingMs)
            {
                if (uint32 const emote = RangedReadyOneShotEmote(player))
                    player->HandleEmoteCommand(emote);
                intent.readyEmoteSent = true;
            }
        }

        if (intent != initial)
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto const found = shard.intents.find(guid);
            if (found != shard.intents.end() && found->second.target == intent.target)
                found->second = intent;
        }
    }
};

class AttackMapCleanup final : public AllMapScript
{
public:
    AttackMapCleanup() : AllMapScript("TwowAutoAttackMapCleanup") {}

    void OnPlayerLeaveAll(Map* /*map*/, Player* player) override
    {
        if (!enabled.load(std::memory_order_relaxed) || !player || !player->GetSession() ||
            player->GetSession()->IsHeadless())
            return;
        Forget(player, true, "left map", true);
    }
};
}

void Addmod_twow_autoattackScripts()
{
    new Settings();
    new RangedCommands();
    new AttackPackets();
    new AttackUpdates();
    new AttackMapCleanup();
}
