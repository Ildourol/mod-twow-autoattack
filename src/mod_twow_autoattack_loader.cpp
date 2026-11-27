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

#include <atomic>
#include <cctype>
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

struct PendingShot
{
    ObjectGuid target;
    Spell const* previous;
    uint32 spellId;
    CurrentSpellTypes slot;
};

struct Intent
{
    ObjectGuid target;
    Spell const* shot = nullptr;
    uint32 shotId = 0;
    CurrentSpellTypes slot = CURRENT_AUTOREPEAT_SPELL;
    uint32 retry = 0;
    uint32 transition = 0;
    SpellCastResult lastFailure = SPELL_CAST_OK;
    bool ranged = false;
    std::optional<PendingShot> pending;
};

std::atomic<bool> enabled{false};
std::atomic<bool> preferencesAvailable{false};
std::atomic<bool> meleePreferencesAvailable{false};
bool debug = false;
uint32 debugGuid = 0;
bool rightClick = true;
bool defaultPreferMelee = true;
bool announce = false;
bool stopOnTargetChange = true;
std::mutex intentsMutex;
std::unordered_map<uint32, Intent> intents;
std::unordered_set<uint32> rangedDisabled;
std::unordered_map<uint32, bool> meleePreferences;

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

bool PreferMelee(uint32 guid)
{
    std::lock_guard<std::mutex> lock(intentsMutex);
    auto const found = meleePreferences.find(guid);
    return found == meleePreferences.end() ? defaultPreferMelee : found->second;
}

bool RangedEnabled(uint32 guid)
{
    std::lock_guard<std::mutex> lock(intentsMutex);
    return rangedDisabled.count(guid) == 0;
}

void CacheRangedPreference(uint32 guid, bool ranged)
{
    std::lock_guard<std::mutex> lock(intentsMutex);
    if (ranged)
        rangedDisabled.erase(guid);
    else
        rangedDisabled.insert(guid);
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

void Forget(Player* player, bool interrupt, char const* reason = "intent cleared", bool stopRangedVictim = false)
{
    Intent intent;
    {
        std::lock_guard<std::mutex> lock(intentsMutex);
        auto const found = intents.find(player->GetGUIDLow());
        if (found == intents.end())
            return;
        intent = found->second;
        intents.erase(found);
    }

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
    intent.ranged = true;
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

class Settings final : public WorldScript
{
public:
    Settings() : WorldScript("TwowAutoAttackSettings", {WORLDHOOK_ON_STARTUP}) {}

    void OnStartup() override
    {
        debug = sConfig.GetBoolDefault("TwowAutoAttack.Debug", false);
        int32 const filter = sConfig.GetIntDefault("TwowAutoAttack.Debug.PlayerGuid", 0);
        debugGuid = filter > 0 ? uint32(filter) : 0;
        rightClick = sConfig.GetBoolDefault("TwowAutoAttack.RightClick", true);
        defaultPreferMelee = sConfig.GetBoolDefault("TwowAutoAttack.CloseRange.Melee", true);
        announce = sConfig.GetBoolDefault("TwowAutoAttack.Announce", false);
        stopOnTargetChange = sConfig.GetBoolDefault("TwowAutoAttack.StopOnTargetChange", true);
        enabled = sConfig.GetBoolDefault("TwowAutoAttack.Enable", false);
        sLog.outInfo("TwowAutoAttack: adaptive attacks %s.", enabled ? "enabled" : "disabled");
        if (enabled)
        {
            std::unique_ptr<QueryResult> table(CharacterDatabase.PQuery(
                "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                "AND TABLE_NAME = 'mod_twow_autoattack_ranged_disabled'"));
            preferencesAvailable = bool(table);
            std::unique_ptr<QueryResult> meleeTable(CharacterDatabase.PQuery(
                "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                "AND TABLE_NAME = 'mod_twow_autoattack_melee_preference'"));
            meleePreferencesAvailable = bool(meleeTable);
        }
    }
};

class RangedCommands final : public AllCommandScript
{
public:
    RangedCommands() : AllCommandScript("TwowAutoAttackCommands") {}

    bool CanExecuteCommand(ChatHandler* handler, char const* command, char const* arguments) override
    {
        if (!handler || !command)
            return true;
        std::string const name = Lowercase(command);
        if (name != "aaf" && name != "rangedauto")
            return true;

        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || session->IsHeadless() || !enabled)
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
            handler->SendSysMessage(RangedEnabled(guid) ? "AAF_RANGED 1" : "AAF_RANGED 0");
            handler->SendSysMessage(PreferMelee(guid) ? "AAF_MELEE 1" : "AAF_MELEE 0");
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
                if (!meleePreferencesAvailable)
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
                std::lock_guard<std::mutex> lock(intentsMutex);
                meleePreferences[guid] = prefer;
            }
            handler->SendSysMessage(PreferMelee(guid) ? "AAF_MELEE 1" : "AAF_MELEE 0");
            return false;
        }

        if (action != "status")
        {
            if (!preferencesAvailable)
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
        if (!enabled || !session)
            return true;

        switch (incoming.GetOpcode())
        {
            case CMSG_ATTACKSWING:
            case CMSG_ATTACKSTOP:
            case CMSG_CANCEL_AUTO_REPEAT_SPELL:
            case CMSG_CAST_SPELL:
            case CMSG_CANCEL_CAST:
            case CMSG_SET_SELECTION:
                break;
            default:
                return true;
        }

        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || session->IsHeadless())
            return true;

        if (incoming.GetOpcode() == CMSG_ATTACKSTOP)
        {
            Forget(player, true, "attack stopped by client");
            return true;
        }

        if (incoming.GetOpcode() == CMSG_CANCEL_AUTO_REPEAT_SPELL)
        {
            bool cancelOwnedRepeat = false;
            {
                std::lock_guard<std::mutex> lock(intentsMutex);
                auto const found = intents.find(player->GetGUIDLow());
                if (found != intents.end())
                {
                    Intent const& intent = found->second;
                    cancelOwnedRepeat = (intent.slot == CURRENT_AUTOREPEAT_SPELL &&
                        (OwnedShot(player, intent) ||
                            (intent.shotId == WandShoot && intent.ranged && !intent.transition && !intent.pending))) ||
                        (intent.pending && intent.pending->slot == CURRENT_AUTOREPEAT_SPELL &&
                            AcceptedShot(player, *intent.pending));
                }
            }
            if (cancelOwnedRepeat)
                Forget(player, false, "ranged repeat cancelled by client", true);
            // A late cancel with an empty repeat slot cannot stop a new intent
            // or an independently timed Shoot/Throw cast.
            return true;
        }

        WorldPacket packet(incoming);
        if (incoming.GetOpcode() == CMSG_SET_SELECTION)
        {
            ObjectGuid selection;
            packet >> selection;
            Intent intent;
            {
                std::lock_guard<std::mutex> lock(intentsMutex);
                auto const found = intents.find(player->GetGUIDLow());
                if (found == intents.end())
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
                    std::lock_guard<std::mutex> lock(intentsMutex);
                    intents.insert_or_assign(player->GetGUIDLow(), intent);
                }
            }
            return true;
        }

        if (incoming.GetOpcode() != CMSG_ATTACKSWING)
        {
            uint32 spellId;
            packet >> spellId;
            if (incoming.GetOpcode() == CMSG_CANCEL_CAST)
            {
                bool cancel = false;
                {
                    std::lock_guard<std::mutex> lock(intentsMutex);
                    auto const found = intents.find(player->GetGUIDLow());
                    if (found != intents.end())
                    {
                        Intent const& intent = found->second;
                        cancel = ((!spellId || spellId == intent.shotId) && OwnedShot(player, intent)) ||
                            (intent.pending && (!spellId || spellId == intent.pending->spellId) &&
                                AcceptedShot(player, *intent.pending));
                    }
                }
                if (cancel)
                    Forget(player, false, "ranged cast cancelled by client", true);
                return true;
            }

            if (!Eligible(player) || !spellId || spellId != WeaponShot(player, spellId) ||
                !RangedEnabled(player->GetGUIDLow()))
                return true;

            SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
            if (!spell || spell->IsPassiveSpell())
                return true;

            SpellCastTargets targets;
            packet >> targets.ReadForCaster(player);
            Unit* target = targets.getUnitTarget();
            if (!ValidTarget(player, target))
                return true;

            CurrentSpellTypes const slot = spell->IsAutoRepeatRangedSpell()
                ? CURRENT_AUTOREPEAT_SPELL : CURRENT_GENERIC_SPELL;
            uint32 const guid = player->GetGUIDLow();
            Intent intent;
            {
                std::lock_guard<std::mutex> lock(intentsMutex);
                auto const found = intents.find(guid);
                if (found != intents.end())
                    intent = found->second;
            }
            AdoptPendingShot(player, intent);
            intent.pending = PendingShot{target->GetObjectGuid(), player->GetCurrentSpell(slot), spellId, slot};
            std::lock_guard<std::mutex> lock(intentsMutex);
            intents.insert_or_assign(guid, intent);
            return true;
        }

        if (!Eligible(player))
            return true;

        ObjectGuid guid;
        packet >> guid;
        Unit* target = guid.IsUnit() ? player->GetMap()->GetUnit(guid) : nullptr;
        if (!ValidTarget(player, target) || player->IsFriendlyTo(target))
            return true;

        if (!rightClick)
        {
            Forget(player, true, "stock attack requested");
            return true;
        }

        uint32 const playerGuid = player->GetGUIDLow();
        uint32 const directShot = RangedEnabled(playerGuid)
            ? DesiredShot(player, target, 0, PreferMelee(playerGuid)) : 0;
        {
            std::lock_guard<std::mutex> lock(intentsMutex);
            auto const found = intents.find(playerGuid);
            if (found != intents.end())
            {
                Intent const& intent = found->second;
                if (intent.pending && intent.pending->target == guid && AcceptedShot(player, *intent.pending))
                    return !directShot;
                if (intent.target == guid && player->GetVictim() == target)
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
            intent.ranged = true;
            Trace(player, guid, "ranged-first attack", directShot);
        }

        Forget(player, true, "attack target changed");
        {
            std::lock_guard<std::mutex> lock(intentsMutex);
            intents.emplace(playerGuid, intent);
        }
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
        if (!enabled || player->GetSession()->IsHeadless())
            return;

        uint32 const guid = player->GetGUIDLow();
        if (preferencesAvailable)
        {
            std::unique_ptr<QueryResult> row(CharacterDatabase.PQuery(
                "SELECT guid FROM mod_twow_autoattack_ranged_disabled WHERE guid = %u", guid));
            CacheRangedPreference(guid, !row);
        }
        if (meleePreferencesAvailable)
        {
            std::unique_ptr<QueryResult> row(CharacterDatabase.PQuery(
                "SELECT prefer_melee FROM mod_twow_autoattack_melee_preference WHERE guid = %u", guid));
            std::lock_guard<std::mutex> lock(intentsMutex);
            if (row)
                meleePreferences[guid] = row->Fetch()[0].GetUInt32() != 0;
            else
                meleePreferences.erase(guid);
        }
        if (announce)
            ChatHandler(player).SendSysMessage("Auto-Attack Forever is enabled. Use .aaf status for your settings.");
    }

    void OnLogout(Player* player) override
    {
        Forget(player, false, "logout");
        std::lock_guard<std::mutex> lock(intentsMutex);
        rangedDisabled.erase(player->GetGUIDLow());
        meleePreferences.erase(player->GetGUIDLow());
    }
    void OnMapChanged(Player* player) override { Forget(player, false, "map changed"); }
    void OnPlayerJustDied(Player* player) override { Forget(player, false, "player died"); }

    void OnSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!enabled || !IsPacedShot(spell->m_spellInfo->Id) || spell->IsAutoRepeat() ||
            spell->IsTriggered() || !player->GetSession() ||
            player->GetSession()->IsHeadless())
            return;

        bool paced = false;
        {
            std::lock_guard<std::mutex> lock(intentsMutex);
            auto const found = intents.find(player->GetGUIDLow());
            if (found != intents.end())
                paced = found->second.slot == CURRENT_GENERIC_SPELL && OwnedShot(player, found->second) == spell;
        }
        if (paced)
            player->ResetAttackTimer(RANGED_ATTACK);
    }

    void OnBeforeUpdate(Player* player, uint32 diff) override
    {
        if (!enabled || !player->GetSession() || player->GetSession()->IsHeadless())
            return;

        uint32 const guid = player->GetGUIDLow();
        Intent intent;
        bool ranged;
        bool preferMelee;
        {
            std::lock_guard<std::mutex> lock(intentsMutex);
            auto const found = intents.find(guid);
            if (found == intents.end())
                return;
            intent = found->second;
            ranged = rangedDisabled.count(guid) == 0;
            auto const preference = meleePreferences.find(guid);
            preferMelee = preference == meleePreferences.end() ? defaultPreferMelee : preference->second;
        }

        if (!Eligible(player))
        {
            Forget(player, true, "player unavailable", true);
            return;
        }

        if (intent.pending)
        {
            AdoptPendingShot(player, intent);
            {
                std::lock_guard<std::mutex> lock(intentsMutex);
                auto const found = intents.find(guid);
                if (found == intents.end())
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
                player->Attack(target, true);
                Trace(player, intent.target, "switched to melee");
            }
            intent.ranged = false;
            intent.retry = 0;
            intent.lastFailure = SPELL_CAST_OK;
        }
        else
        {
            intent.ranged = true;
            if (player->HasUnitState(UNIT_STAT_MELEE_ATTACKING))
            {
                player->ClearUnitState(UNIT_STAT_MELEE_ATTACKING);
                player->SendMeleeAttackStop(target);
                Trace(player, intent.target, "switched to ranged", desired);
            }
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
                        intent.lastFailure = geometry;
                        intent.retry = TransientRetryMs;
                    }
                    else
                    {
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
        }

        std::lock_guard<std::mutex> lock(intentsMutex);
        auto const found = intents.find(guid);
        if (found != intents.end() && found->second.target == intent.target)
            found->second = intent;
    }
};

class AttackMapCleanup final : public AllMapScript
{
public:
    AttackMapCleanup() : AllMapScript("TwowAutoAttackMapCleanup") {}

    void OnPlayerLeaveAll(Map* /*map*/, Player* player) override
    {
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
