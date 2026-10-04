/*
 * mod-lfg-expansion
 *
 * The Dungeon Finder never sends a player to a dungeon from an expansion they
 * have not reached.
 *
 * Random Classic stops at 58 and Random Burning Crusade starts at 59 in
 * LFGDungeons.dbc, so a level 59-60 is only offered TBC. For a vanilla
 * character under mod-individual-progression that is worse than wrong: IP
 * turns it away at the entrance of every Outland instance.
 *
 * The levels can NOT be fixed in data alone: the client's LFDFrame.lua only
 * shows a random dungeon if the player's level is inside the CLIENT's DBC
 * range (isRandomDungeonDisplayable). So the random id is swapped on queue in
 * OnPlayerQueueRandomDungeon, the same trick as mod-rdf-expansion -- only per
 * group instead of globally. The player picks "Random Burning Crusade", queues
 * for Random Classic, and the queue status shows Random Classic afterwards
 * (JoinLfg stores the swapped id with SetSelectedDungeons).
 *
 * The player lock is the core's own. LFGMgr::InitializeLockedDungeons works
 * out every player's locked dungeons on login and on every level change and
 * calls OnInitializeLockedDungeons for each dungeon; GetCompatibleDungeons()
 * removes the locked ones on join, also after a random dungeon is expanded.
 *
 * mod-individual-progression and mod-playerbots are optional. With IP, a
 * character's expansion is its IP tier; without it, its level. With
 * playerbots, random bots are left alone: they have no expansion of their own.
 */

#include "Config.h"
#include "Group.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#if __has_include("IndividualProgression.h")
#include "IndividualProgression.h"
#define LFGX_WITH_INDIVIDUAL_PROGRESSION 1
#endif

#if __has_include("PlayerbotAIConfig.h")
#include "PlayerbotAIConfig.h"
#define LFGX_WITH_PLAYERBOTS 1
#endif

#include "lfg_expansion_rules.h"

namespace
{
    LfgExpansion::Settings g_settings;

    // Playerbots' own definition of a random bot: the account is in
    // randomBotAccounts. That list is filled once at startup, so it is safe
    // to read from the map threads, where a bot's own level-up happens.
    // RandomPlayerbotMgr::IsRandomBot() is not -- it looks in currentBots,
    // which the world thread changes all the time.
    bool IsRandomBot([[maybe_unused]] Player* player)
    {
#ifdef LFGX_WITH_PLAYERBOTS
        WorldSession* session = player->GetSession();
        return session && session->IsHeadless() && sPlayerbotAIConfig.IsInRandomAccountList(session->GetAccountId());
#else
        return false;
#endif
    }

    // mod-individual-progression's expansion for the character, or -1 if IP
    // does not control the account. isNormalAccount() looks the account name
    // up in the database and builds a regex per call, so it is only called
    // when the level alone does not decide (see PlayerExpansion).
    int IpTier([[maybe_unused]] Player* player)
    {
#ifdef LFGX_WITH_INDIVIDUAL_PROGRESSION
        if (!sIndividualProgression->enabled || !sIndividualProgression->isNormalAccount(player))
            return -1;
        if (sIndividualProgression->hasPassedProgression(player, PROGRESSION_TBC_TIER_5))
            return LfgExpansion::EXPANSION_WOTLK;
        if (sIndividualProgression->hasPassedProgression(player, PROGRESSION_PRE_TBC))
            return LfgExpansion::EXPANSION_TBC;
        return LfgExpansion::EXPANSION_CLASSIC;
#else
        return -1;
#endif
    }

    uint8 PlayerExpansion(Player* player, uint8 level)
    {
        // Already in the highest expansion by level: IP can only lift.
        if (LfgExpansion::ExpansionForLevel(g_settings, level) == LfgExpansion::EXPANSION_WOTLK)
            return LfgExpansion::EXPANSION_WOTLK;
        return LfgExpansion::PlayerExpansion(g_settings, level, IpTier(player));
    }

    // InitializeLockedDungeons() calls the lock hook once per dungeon with the
    // same player, and IpTier() costs two synchronous auth database lookups
    // and two regexes per call. So the expansion is worked out once per pass
    // and forgotten in OnAfterInitializeLockedDungeons -- the IP tier can
    // change without the level changing. thread_local, because the locks are
    // worked out both in the world thread (login) and in the map threads
    // (level change).
    struct LockPass
    {
        ObjectGuid guid;
        uint8 level = 0;
        uint8 expansion = 0;
    };
    thread_local LockPass t_lockPass;

    uint8 PlayerExpansionForLockPass(Player* player, uint8 level)
    {
        if (t_lockPass.guid != player->GetGUID() || t_lockPass.level != level)
            t_lockPass = { player->GetGUID(), level, PlayerExpansion(player, level) };
        return t_lockPass.expansion;
    }
}

class LfgExpansionWorld : public WorldScript
{
public:
    LfgExpansionWorld() : WorldScript("LfgExpansionWorld") { }

    // Read here and not in the hooks: GetOption() logs "Missing property" on
    // every call when a key is missing, and the lock hook runs ~300 times per
    // character per level change.
    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_settings.enabled         = sConfigMgr->GetOption<bool>("LfgExpansion.Enable", true);
        g_settings.classicMaxLevel = uint8(sConfigMgr->GetOption<uint32>("LfgExpansion.ClassicMaxLevel", 60));
        g_settings.tbcMaxLevel     = uint8(sConfigMgr->GetOption<uint32>("LfgExpansion.TbcMaxLevel", 70));

#ifdef LFGX_WITH_INDIVIDUAL_PROGRESSION
        char const* const ip = "found";
#else
        char const* const ip = "not found";
#endif
#ifdef LFGX_WITH_PLAYERBOTS
        char const* const playerbots = "found";
#else
        char const* const playerbots = "not found";
#endif

        LOG_INFO("module", "LfgExpansion: {} (vanilla up to level {}, TBC up to {}; individual progression {}, playerbots {})",
                 g_settings.enabled ? "enabled" : "disabled",
                 g_settings.classicMaxLevel, g_settings.tbcMaxLevel, ip, playerbots);
    }
};

class LfgExpansionGlobal : public GlobalScript
{
public:
    LfgExpansionGlobal() : GlobalScript("LfgExpansionGlobal", {
        GLOBALHOOK_ON_INITIALIZE_LOCKED_DUNGEONS,
        GLOBALHOOK_ON_AFTER_INITIALIZE_LOCKED_DUNGEONS
    }) { }

    void OnInitializeLockedDungeons(Player* player, uint8& level, uint32& lockData, lfg::LFGDungeonData const* dungeon) override
    {
        // A dungeon the core already locked keeps its own reason.
        if (lockData || !dungeon || !player || !g_settings.enabled)
            return;

        // Random bots have no expansion of their own.
        if (IsRandomBot(player))
            return;

        // Cheapest check first: only a dungeon from a later expansion than the
        // level's can be locked, and only then is IP looked up.
        bool const isRandomEntry = dungeon->type == lfg::LFG_TYPE_RANDOM;
        if (isRandomEntry || dungeon->expansion <= LfgExpansion::ExpansionForLevel(g_settings, level))
            return;

        if (LfgExpansion::IsPlayerLocked(g_settings, PlayerExpansionForLockPass(player, level), dungeon->expansion, isRandomEntry))
            lockData = lfg::LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION;
    }

    void OnAfterInitializeLockedDungeons(Player* /*player*/) override
    {
        t_lockPass = {};
    }
};

class LfgExpansionPlayer : public PlayerScript
{
public:
    LfgExpansionPlayer() : PlayerScript("LfgExpansionPlayer", {
        PLAYERHOOK_ON_QUEUE_RANDOM_DUNGEON
    }) { }

    // Called in LFGMgr::JoinLfg with the player who queues -- the group
    // leader, if there is a group -- before the random id is expanded into
    // dungeons.
    void OnPlayerQueueRandomDungeon(Player* player, uint32& rDungeonId) override
    {
        if (!player || !g_settings.enabled)
            return;

        if (IsRandomBot(player))
            return;

        uint8 groupExpansion = PlayerExpansion(player, player->GetLevel());
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player || IsRandomBot(member))
                    continue;
                groupExpansion = LfgExpansion::GroupExpansion({ groupExpansion, PlayerExpansion(member, member->GetLevel()) });
            }
        }

        uint32 const swapped = LfgExpansion::RandomFor(g_settings, rDungeonId, groupExpansion);
        if (swapped == rDungeonId)
            return;

        LOG_INFO("module", "LfgExpansion: {} (level {}) queues random {} -> {} (group expansion {})",
                 player->GetName(), player->GetLevel(), rDungeonId, swapped, groupExpansion);
        rDungeonId = swapped;
    }
};

void AddLfgExpansionScripts()
{
    new LfgExpansionWorld();
    new LfgExpansionGlobal();
    new LfgExpansionPlayer();
}
