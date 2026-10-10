#include "rw_core.hpp"

#include <cstdlib>

#include <pisex/keyvalues.hpp>

namespace rw {
namespace {

// One "player count" block of the config, e.g. "7" { "weapon_awp" "2" ... }. The original walked
// the children with GetFirstSubKey()/GetNextKey(), which visits *both* blocks and value nodes -
// the weapon entries are value nodes, so the FOR_EACH_* macros (block-only / value-only) would
// each skip half of them.
void ReadThresholdBlock(std::map<int, std::map<std::string, int>>& out, pisex::KeyValues* block) {
	if (block == nullptr) {
		return;
	}
	for (pisex::KeyValues* playerKey = block->GetFirstSubKey(); playerKey != nullptr;
		 playerKey = playerKey->GetNextKey()) {
		const int count = std::atoi(playerKey->GetName()); // the original's atoi(pKVPlayer->GetName())
		for (pisex::KeyValues* weaponKey = playerKey->GetFirstSubKey(); weaponKey != nullptr;
			 weaponKey = weaponKey->GetNextKey()) {
			out[count][weaponKey->GetName()] = weaponKey->GetInt(nullptr);
		}
	}
}

} // namespace

void ParseConfig(Config& out, pisex::KeyValues& root, std::string_view mapName) {
	out.restrictions.clear();
	out.typePlayers = root.GetInt("type_players", 1);
	out.typeWeapons = root.GetInt("type_weapons", 1);
	out.unblockType = root.GetInt("unblock_type", 0);
	out.specPlayers = root.GetBool("spec_players", false);
	out.intervalMessage = root.GetInt("interval_message", 3);
	out.blockSound = std::string(root.GetString("block_sound", ""));

	ReadThresholdBlock(out.restrictions, root.FindKey("all"));
	if (!mapName.empty()) {
		ReadThresholdBlock(out.restrictions, root.FindKey(std::string(mapName).c_str()));
	}
}

int BestFitThreshold(const std::map<int, std::map<std::string, int>>& restrictions, int playersCount) {
	// The original iterated the (sorted) std::map and kept the largest key <= playersCount.
	int bestFit = -1;
	for (const auto& [count, weapons] : restrictions) {
		(void)weapons;
		if (count <= playersCount) {
			bestFit = count;
		}
	}
	return bestFit;
}

int FindLimit(const Config& config, const std::string& weapon, int playersCount) {
	const int bestFit = BestFitThreshold(config.restrictions, playersCount);
	if (bestFit == -1) {
		return kNotRestricted;
	}
	const auto threshold = config.restrictions.find(bestFit);
	if (threshold == config.restrictions.end()) {
		return kNotRestricted;
	}
	const auto entry = threshold->second.find(weapon);
	if (entry == threshold->second.end()) {
		return kNotRestricted;
	}
	// -1 is the original's "explicitly allowed" value, i.e. no restriction.
	return entry->second == -1 ? kNotRestricted : entry->second;
}

Decision Evaluate(const Config& config, const std::string& weapon, int playersCount, int weaponsOfWeaponCount) {
	const int limit = FindLimit(config, weapon, playersCount);
	// The original allowed when the count was below the limit or the limit was -1.
	if (limit == kNotRestricted || weaponsOfWeaponCount < limit) {
		return {};
	}
	return Decision{Verdict::Restricted, limit};
}

const char* WeaponByDefIndex(int defIndex) {
	switch (defIndex) {
		case 1: return "weapon_deagle";
		case 2: return "weapon_elite";
		case 3: return "weapon_fiveseven";
		case 4: return "weapon_glock";
		case 7: return "weapon_ak47";
		case 8: return "weapon_aug";
		case 9: return "weapon_awp";
		case 10: return "weapon_famas";
		case 11: return "weapon_g3sg1";
		case 13: return "weapon_galilar";
		case 14: return "weapon_m249";
		case 16: return "weapon_m4a1";
		case 17: return "weapon_mac10";
		case 19: return "weapon_p90";
		case 23: return "weapon_mp5sd";
		case 24: return "weapon_ump45";
		case 25: return "weapon_xm1014";
		case 26: return "weapon_bizon";
		case 27: return "weapon_mag7";
		case 28: return "weapon_negev";
		case 29: return "weapon_sawedoff";
		case 30: return "weapon_tec9";
		case 31: return "weapon_taser";
		case 32: return "weapon_hkp2000";
		case 33: return "weapon_mp7";
		case 34: return "weapon_mp9";
		case 35: return "weapon_nova";
		case 36: return "weapon_p250";
		case 38: return "weapon_scar20";
		case 39: return "weapon_sg556";
		case 40: return "weapon_ssg08";
		case 60: return "weapon_m4a1_silencer";
		case 61: return "weapon_usp_silencer";
		case 63: return "weapon_cz75a";
		case 64: return "weapon_revolver";
		default: return "weapon_knife";
	}
}

} // namespace rw
