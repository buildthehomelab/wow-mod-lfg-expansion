# mod-lfg-expansion

An [AzerothCore](https://www.azerothcore.org) module that keeps the Dungeon
Finder inside the expansion a character has reached. Per player, with no
config switch when some of your players are in vanilla and others in TBC.

- A **random dungeon** from a later expansion is swapped on queue for the
  group's own: a level 60 who picks *Random Burning Crusade* queues for
  *Random Classic*.
- **Specific dungeons** from a later expansion are shown as locked
  (`LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION`).

Works on its own (a character's expansion is its level) and together with
[mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
(a character's expansion is its progression tier).

## Why

From the client's own `LFGDungeons.dbc`:

| Random dungeon | Levels | Expansion |
|---|---|---|
| 258 Random Classic | 15-58 | vanilla |
| 259 Random Burning Crusade | 59-68 | TBC |
| 260 Random Burning Crusade Heroic | 70-73 | TBC |
| 261 Random Lich King | 69-80 | WotLK |
| 262 Random Lich King Heroic | 80-83 | WotLK |

A level 59-60 can only pick Random Burning Crusade, and a level 69-70 is
offered Random Lich King. Under mod-individual-progression that is worse than
wrong: IP turns a vanilla character away at the entrance of every Outland
instance, and a TBC character at Northrend.

### Why not just fix the levels in `lfgdungeons_dbc`

The server would take it: `LFGDungeons.dbc` can be overridden in SQL. But the
client's `Interface/FrameXML/LFDFrame.lua` only shows a random dungeon if the
player's level is inside the **client's** DBC range:

```lua
local function isRandomDungeonDisplayable(id)
	local name, typeID, minLevel, maxLevel, _, _, _, expansionLevel = GetLFGDungeonInfo(id);
	local myLevel = UnitLevel("player");
	return myLevel >= minLevel and myLevel <= maxLevel and EXPANSION_LEVEL >= expansionLevel;
end
```

Random Classic would still be hidden at 59-60, and without Random Burning
Crusade a level 60 would have no random to pick. Changing the client's DBC
means patching every client.

## How it works

### Random dungeons are swapped on queue

`OnPlayerQueueRandomDungeon` is called in `LFGMgr::JoinLfg` with the player who
queues (the group leader), **before** the random id is expanded into dungeons.
It is the same trick `mod-rdf-expansion` uses globally; here it happens per
group:

1. The group's expansion is the **lowest** among its players.
2. If the chosen random is from a later expansion, it is swapped down: a
   vanilla group that picks Random Burning Crusade queues for Random Classic.
   Heroic stays heroic where the expansion has one.

The player still picks "Random Burning Crusade Dungeon" in the dropdown -- it is
the only one the client shows at 59-60 -- but the queue then reads Random
Classic, because `JoinLfg` stores the swapped id with `SetSelectedDungeons`.
The reward is Random Classic's: `GetRandomDungeonReward()` falls back to the
last row when the level is above the table's highest `maxLevel`.

### Specific dungeons are locked

`LFGMgr::InitializeLockedDungeons()` works out every character's locked
dungeons on login and on every level change, and calls
`OnInitializeLockedDungeons` for each dungeon. A specific dungeon from a later
expansion than the player's gets `LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION`, and
the client shows it as locked. The random entries are **not** locked -- a
locked random is greyed out, and then the swap above could never happen.

On join, `GetCompatibleDungeons()` removes locked dungeons, so matchmaking can
never place anyone in them. Random pools are expanded before the locks are
checked, so they are covered.

A dungeon's expansion is the `ExpansionLevel` column of `LFGDungeons.dbc`, not
its level range: Stratholme is vanilla even though its range reaches above 60.

## A character's expansion

| Account | Expansion |
|---|---|
| controlled by mod-individual-progression | IP's tier: vanilla until Naxxramas-40 (`PROGRESSION_PRE_TBC`), TBC until Sunwell (`PROGRESSION_TBC_TIER_5`), then WotLK |
| excluded by IP, IP disabled, or not installed | the level: up to and including 60 vanilla, up to and including 70 TBC |

The level is a floor for IP characters, because IP holds a vanilla character
at 60 and a TBC character at 70. So IP is only asked when the level alone does
not decide. `isNormalAccount()` looks up the account name in the auth database
and builds a regex per call, and the lock hook runs once per dungeon, so the
expansion is worked out once per pass of `InitializeLockedDungeons()`.

| Player | Picks | Gets |
|---|---|---|
| vanilla (IP), level 60 | Random Burning Crusade | Random Classic; Hellfire Ramparts locked |
| TBC (IP, Naxx40 cleared), level 60 | Random Burning Crusade | Random Burning Crusade |
| TBC, level 69-70 | Random Lich King | Random Burning Crusade; Utgarde Keep locked |
| WotLK | anything | unchanged |

## Optional modules

Both are detected at compile time with `__has_include`; the startup line says
which were found.

- **mod-individual-progression** (`IndividualProgression.h`): the IP tier
  decides, as above.
- **mod-playerbots** (`PlayerbotAIConfig.h`): random bots (accounts in
  `randomBotAccounts`) are never locked or counted -- they have no expansion
  of their own. Your own altbots count like players. Which bots playerbots
  sends to a queue is playerbots' own decision.

A module that is present in `modules/` but disabled in CMake still has its
header found, and the build then fails at link time. Remove the directory
instead.

## Installation

1. Clone into `modules/` of your AzerothCore source tree and rebuild:
   ```bash
   cd azerothcore-wotlk/modules
   git clone https://github.com/MekBits/mod-lfg-expansion.git
   ```
2. Copy `conf/mod_lfg_expansion.conf.dist` to `mod_lfg_expansion.conf` in your
   config directory (the defaults also apply without it).

`Server.log` after start:
`LfgExpansion: enabled (vanilla up to level 60, TBC up to 70; individual progression found, playerbots not found)`,
and for every swapped queue
`LfgExpansion: <name> (level 60) queues random 259 -> 258 (group expansion 0)`.

To check in game as a vanilla level 60: pick Random Burning Crusade; the queue
should read Random Classic and the dungeon should be vanilla. Hellfire
Ramparts should show as locked under Specific Dungeons.

## Configuration

| Key | Default | |
|---|---|---|
| `LfgExpansion.Enable` | 1 | enable the module |
| `LfgExpansion.ClassicMaxLevel` | 60 | highest vanilla level for a character without an IP tier |
| `LfgExpansion.TbcMaxLevel` | 70 | highest TBC level for a character without an IP tier |

## Tests

```bash
./test/run.sh
```

Needs only g++ and tests the rules. The `core-build` workflow compiles the
module against AzerothCore.

## License

GNU Affero General Public License v3.0, see [LICENSE](LICENSE).
