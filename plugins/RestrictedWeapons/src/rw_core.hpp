// rw_core — engine-free core of the RestrictedWeapons port.
//
// Everything the original LoadConfigs() / CanAcquireListener() computed without touching the
// engine lives here, so it is unit-testable (tests/rw_core_tests.cpp parses the *live* config file
// and exercises the decision rules of ref/src/cs2-restricted_weapons/Core/RestrictedWeapons.cpp):
//
//   * the config model of restricted_weapons.ini ("all" + <map> blocks, threshold -> weapon -> limit),
//   * the "best fit" threshold selection (largest player-count key <= current player count),
//   * the restriction decision (weapon limit vs. counted weapons),
//   * the defindex -> weapon classname table (GetWeaponByDefIndex, default "weapon_knife").
//
// The engine glue (players/weapons counting, the CanAcquire hook, chat, sound) stays in
// src/plugin.cpp.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace pisex {
class KeyValues;
}

namespace rw {

// CS2 has 64 player slots.
inline constexpr int kMaxSlots = 64;

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; EntIndexToEntHandle
// answers -1 for "no entity"). The sign means nothing - once the engine's serial counter passes
// 65535 bit 31 is set and the handle is a negative int; a `handle > 0` test silently drops those
// controllers (the block sound is not played then).
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// AcquireResult::Type of the Pisex utils hook (ref/pisex/cs2-menus/include/utils.h); the values
// are the engine ABI of CCSPlayer_ItemServices::CanAcquire's return and must not change.
enum class AcquireResult : int32_t {
	Allowed = 0,
	InvalidItem,
	AlreadyOwned,
	AlreadyPurchased,
	ReachedGrenadeTypeLimit,
	ReachedGrenadeTotalLimit,
	NotAllowedByTeam,
	NotAllowedByMap,
	NotAllowedByMode,
	NotAllowedForPurchase,
	NotAllowedByProhibition,
};

// AcquireMethod::Type of the same header.
enum class AcquireMethod : int32_t {
	PickUp = 0,
	Buy,
};

// The model of restricted_weapons.ini. Defaults are the defaults of LoadConfigs():
// type_players 1 (all players), type_weapons 1 (all weapons), unblock_type 0, spec_players false,
// interval_message 3.
struct Config {
	std::string blockSound;				 // "block_sound" (soundevent name; empty = no sound)
	int typePlayers = 1;				 // 1 = count all players, 2 = count the team
	bool specPlayers = false;			 // count spectators too (type_players == 1)
	int typeWeapons = 1;				 // 1 = count all weapons, 2 = count the team's weapons
	int unblockType = 0;				 // 0 = a hook listener unblocks always, 1 = only when limit > 0
	int intervalMessage = 3;			 // seconds between two "blocked" messages per player
	// Player-count threshold -> weapon classname -> allowed count. -1 = "explicitly allowed".
	// Built from the "all" block first, then the <map> block, which overwrites entries (the
	// original's g_mRestrictedWeapons[count][weapon] = value).
	std::map<int, std::map<std::string, int>> restrictions;
};

// Parses the loaded KeyValues tree of restricted_weapons.ini (the root block is named
// "RestrictedWeapons"). `mapName` selects the map-specific block; pass "" to read only "all".
// Mirrors LoadConfigs() one to one, including the atoi() of the threshold key names.
void ParseConfig(Config& out, pisex::KeyValues& root, std::string_view mapName);

// The threshold the original picked: the largest key that is <= playersCount, -1 when there is
// none (no restriction applies then).
int BestFitThreshold(const std::map<int, std::map<std::string, int>>& restrictions, int playersCount);

enum class Verdict {
	Allowed,
	Restricted,
};

struct Decision {
	Verdict verdict = Verdict::Allowed;
	int limit = 0; // the configured count of the weapon (only meaningful when Restricted)
};

// The limit the config gives `weapon` at `playersCount`, or kNotRestricted when no restriction
// applies to it: no threshold fits, the weapon is not listed for the fitting one, or it is listed
// as -1 (the original's "explicitly allowed"). Split out of Evaluate so the caller can skip
// counting the weapons of a weapon that is not restricted at all - the count needs every player's
// weapon list, which the original (CanAcquireListener -> GetWeaponCount) only paid for a listed
// weapon.
inline constexpr int kNotRestricted = -1;
int FindLimit(const Config& config, const std::string& weapon, int playersCount);

// CanAcquireListener's rule: the weapon is allowed when the threshold-map has no entry for it, or
// when the counted weapons are below the limit, or when the limit is -1. Otherwise Restricted.
Decision Evaluate(const Config& config, const std::string& weapon, int playersCount, int weaponsOfWeaponCount);

// GetWeaponByDefIndex() of the original: the classname of a weapon defindex; unknown indexes (and
// the default branch of the original's switch) map to "weapon_knife".
const char* WeaponByDefIndex(int defIndex);

} // namespace rw
