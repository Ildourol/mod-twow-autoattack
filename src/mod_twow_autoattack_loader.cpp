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
#include <memory>
#include <mutex>
#include <sstream>
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

struct Intent
{
    ObjectGuid target;
    Spell const* shot = nullptr;
    uint32 shotId = 0;
    CurrentSpellTypes slot = CURRENT_AUTOREPEAT_SPELL;
    uint32 retry = 0;
    uint32 transition = 0;
    bool ranged = false;
    bool awaitingNativeShot = false;
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
    if (current && (current == intent.shot || intent.awaitingNativeShot) &&
        current->m_spellInfo->Id == intent.shotId &&
        current->m_targets.getUnitTargetGuid() == intent.target)
        return current;
    return nullptr;
}

void Forget(Player* player, bool interrupt, char const* reason = "intent cleared")
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
    if (interrupt && OwnedShot(player, intent))
        player->InterruptSpell(intent.slot);
}

bool ValidTarget(Player* player, Unit* target)
{
    return target && target != player && target->IsInWorld() &&
        !target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING | UNIT_FLAG_NOT_SELECTABLE) &&
        player->IsValidAttackTarget(target);
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
        if (!handler || !command || (std::string(command) != "aaf" && std::string(command) != "rangedauto"))
            return true;

        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !enabled)
        {
            handler->SendSysMessage("Auto-Attack Forever is unavailable.");
            return false;
        }

        std::istringstream input(arguments ? arguments : "");
        std::string section, action, extra;
        uint32 const guid = player->GetGUIDLow();
        if (!(input >> section) || (section == "status" && !(input >> extra)))
        {
            handler->SendSysMessage(RangedEnabled(guid) ? "AAF_RANGED 1" : "AAF_RANGED 0");
            handler->SendSysMessage(PreferMelee(guid) ? "AAF_MELEE 1" : "AAF_MELEE 0");
            return false;
        }

        if (!(input >> action) || (section != "ranged" && section != "melee") ||
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
        if (!Eligible(player))
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
                    cancelOwnedRepeat = intent.slot == CURRENT_AUTOREPEAT_SPELL && OwnedShot(player, intent);
                }
            }
            if (cancelOwnedRepeat)
                Forget(player, false, "ranged repeat cancelled by client");
            // A late cancel with an empty repeat slot cannot stop a new intent
            // or an independently timed Shoot/Throw cast.
            return true;
        }

        WorldPacket packet(incoming);
        if (incoming.GetOpcode() == CMSG_SET_SELECTION)
        {
            if (!stopOnTargetChange || player->GetSelectionGuid().IsEmpty())
                return true;
            ObjectGuid selection;
            packet >> selection;
            bool clear = false;
            {
                std::lock_guard<std::mutex> lock(intentsMutex);
                auto const found = intents.find(player->GetGUIDLow());
                if (found != intents.end())
                {
                    Intent const& intent = found->second;
                    clear = (intent.ranged || intent.awaitingNativeShot) &&
                        player->GetSelectionGuid() == intent.target && selection != intent.target;
                }
            }
            if (clear)
                Forget(player, true, "ranged target selection changed");
            return true;
        }

        if (incoming.GetOpcode() != CMSG_ATTACKSWING)
        {
            uint32 spellId;
            packet >> spellId;
            if (incoming.GetOpcode() == CMSG_CAST_SPELL && spellId == WeaponShot(player, spellId) &&
                RangedEnabled(player->GetGUIDLow()))
            {
                SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
                if (!spell || spell->IsPassiveSpell())
                    return true;

                SpellCastTargets targets;
                packet >> targets.ReadForCaster(player);
                Unit* target = targets.getUnitTarget();
                if (!ValidTarget(player, target))
                    return true;

                Forget(player, true);
                Intent intent{target->GetObjectGuid()};
                intent.shotId = spellId;
                intent.slot = spell->IsAutoRepeatRangedSpell() ? CURRENT_AUTOREPEAT_SPELL : CURRENT_GENERIC_SPELL;
                intent.awaitingNativeShot = true;
                std::lock_guard<std::mutex> lock(intentsMutex);
                intents.emplace(player->GetGUIDLow(), intent);
                // Adopt the cast only after the stock handler has accepted it.
                return true;
            }

            Intent intent;
            {
                std::lock_guard<std::mutex> lock(intentsMutex);
                auto const found = intents.find(player->GetGUIDLow());
                if (found == intents.end())
                    return true;
                intent = found->second;
            }

            if (incoming.GetOpcode() == CMSG_CANCEL_CAST)
            {
                if (spellId == intent.shotId && intent.slot == CURRENT_GENERIC_SPELL)
                    Forget(player, false);
                return true;
            }

            SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
            if (!spell || !player->HasActiveSpell(spellId))
                return true;
            if (spell->IsAutoRepeatRangedSpell() || spellId == WeaponShot(player))
                Forget(player, true);
            else if (intent.slot == CURRENT_GENERIC_SPELL && OwnedShot(player, intent))
                player->InterruptSpell(CURRENT_GENERIC_SPELL);
            return true;
        }

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
            if (found != intents.end() && found->second.target == guid &&
                player->GetVictim() == target)
                return !directShot;
        }

        Forget(player, true);
        Intent intent{guid};
        if (directShot)
        {
            // Establish the native victim without starting the melee loop.
            player->Attack(target, false);
            if (player->GetVictim() != target)
                return true;
            intent.ranged = true;
            Trace(player, guid, "ranged-first attack", directShot);
        }

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
        {PLAYERHOOK_ON_BEFORE_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_MAP_CHANGED}) {}

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
            if (row)
            {
                std::lock_guard<std::mutex> lock(intentsMutex);
                meleePreferences[guid] = row->Fetch()[0].GetUInt32() != 0;
            }
        }
        if (announce)
            ChatHandler(player).SendSysMessage("Auto-Attack Forever is enabled. Use .aaf status for your settings.");
    }

    void OnLogout(Player* player) override
    {
        Forget(player, true, "logout");
        std::lock_guard<std::mutex> lock(intentsMutex);
        rangedDisabled.erase(player->GetGUIDLow());
        meleePreferences.erase(player->GetGUIDLow());
    }
    void OnMapChanged(Player* player) override { Forget(player, true); }

    void OnBeforeUpdate(Player* player, uint32 diff) override
    {
        if (!enabled)
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

        Unit* target = Eligible(player) ? player->GetMap()->GetUnit(intent.target) : nullptr;
        if (!ValidTarget(player, target))
        {
            Forget(player, true);
            return;
        }

        if (intent.awaitingNativeShot)
        {
            Spell* shot = player->GetCurrentSpell(intent.slot);
            if (!shot || shot->m_spellInfo->Id != intent.shotId ||
                shot->m_targets.getUnitTargetGuid() != intent.target)
            {
                Forget(player, false);
                return;
            }

            if (player->GetVictim() != target)
                player->Attack(target, false);
            if (player->GetVictim() == target)
            {
                intent.shot = shot;
                intent.ranged = true;
                intent.awaitingNativeShot = false;
                if (intent.slot == CURRENT_GENERIC_SPELL)
                    player->ResetAttackTimer(RANGED_ATTACK);
            }
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
            player->InterruptSpell(intent.slot);
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
            if (!owned && !intent.retry && !intent.transition && !player->IsMoving() &&
                !player->IsNonMeleeSpellCasted(false, false, true))
            {
                SpellEntry const* info = sSpellMgr.GetSpellEntry(desired);
                // Native repeats enforce the weapon timer when firing. Single casts need
                // the same timer check here because each shot is a separate spell.
                if (info && (info->IsAutoRepeatRangedSpell() || player->IsAttackReady(RANGED_ATTACK)))
                {
                    bool const repeats = info->IsAutoRepeatRangedSpell();
                    CurrentSpellTypes const slot = repeats ? CURRENT_AUTOREPEAT_SPELL : CURRENT_GENERIC_SPELL;
                    SpellCastTargets targets;
                    targets.setUnitTarget(target);
                    Spell* spell = new Spell(player, info, false);
                    SpellCastResult const result = spell->prepare(targets);
                    Trace(player, intent.target, "ranged cast attempt", desired, uint32(result));
                    if (result == SPELL_CAST_OK)
                    {
                        if (!repeats)
                            player->ResetAttackTimer(RANGED_ATTACK);
                        if (player->GetCurrentSpell(slot) == spell)
                        {
                            intent.shot = spell;
                            intent.shotId = desired;
                            intent.slot = slot;
                        }
                    }
                    else
                    {
                        switch (result)
                        {
                            case SPELL_FAILED_OUT_OF_RANGE:
                            case SPELL_FAILED_TOO_CLOSE:
                            case SPELL_FAILED_UNIT_NOT_INFRONT:
                            case SPELL_FAILED_LINE_OF_SIGHT:
                            case SPELL_FAILED_MOVING:
                            case SPELL_FAILED_SPELL_IN_PROGRESS:
                                intent.retry = TransientRetryMs;
                                break;
                            default:
                                intent.retry = RetryMs;
                                break;
                        }
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
}

void Addmod_twow_autoattackScripts()
{
    new Settings();
    new RangedCommands();
    new AttackPackets();
    new AttackUpdates();
}
