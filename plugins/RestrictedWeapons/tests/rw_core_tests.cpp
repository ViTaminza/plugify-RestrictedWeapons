// Tests for plugins/RestrictedWeapons/src/rw_core.hpp - the engine-free core of the port
// (config parsing, threshold selection, the restriction decision and the defindex table).
//
// The main input is a real restricted_weapons.ini (passed as RW_TEST_CONFIG), so the parser
// is exercised against the real file, including its tabs/CRLF-free quirks and trailing comments.
//
// Linked against pisexcompat (the KV1 parser), which references the plg::* globals of a real
// plugin; they are defined here as null pointers, the state the library has to survive.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <plg/plugin.hpp>

#include <pisex/keyvalues.hpp>

#include "rw_core.hpp"

namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s\n  expected: %d\n  actual:   %d\n", what.c_str(), expected, actual);
	}
}

void CheckEqStr(const std::string& actual, const std::string& expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s\n  expected: [%s]\n  actual:   [%s]\n", what.c_str(), expected.c_str(),
					actual.c_str());
	}
}

std::string ReadFileOrEmpty(const char* path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return {};
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

bool ParseInto(rw::Config& config, const std::string& text, const std::string& mapName) {
	pisex::KeyValues kv("RestrictedWeapons");
	if (!kv.LoadFromBuffer(text)) {
		return false;
	}
	rw::ParseConfig(config, kv, mapName);
	return true;
}

void TestLiveConfig() {
	const std::string text = ReadFileOrEmpty(RW_TEST_CONFIG);
	Check(!text.empty(), "the live restricted_weapons.ini is readable");
	if (text.empty()) {
		return;
	}
	rw::Config config;
	Check(ParseInto(config, text, ""), "the live config parses");

	// Scalar settings of the real file.
	CheckEqStr(config.blockSound, "Buttons.snd8", "block_sound");
	CheckEq(config.typePlayers, 2, "type_players");
	Check(config.specPlayers == false, "spec_players");
	CheckEq(config.typeWeapons, 2, "type_weapons");
	CheckEq(config.unblockType, 0, "unblock_type");
	CheckEq(config.intervalMessage, 3, "interval_message");
	CheckEq(static_cast<int>(config.restrictions.size()), 5, "five player-count thresholds");

	// The threshold blocks and their weapon entries.
	const auto at = [&config](int threshold, const char* weapon) -> int {
		const auto thresholdIt = config.restrictions.find(threshold);
		if (thresholdIt == config.restrictions.end()) {
			return -1000;
		}
		const auto weaponIt = thresholdIt->second.find(weapon);
		if (weaponIt == thresholdIt->second.end()) {
			return -1001;
		}
		return weaponIt->second;
	};
	CheckEq(at(0, "weapon_awp"), 0, "threshold 0: weapon_awp");
	CheckEq(at(4, "weapon_g3sg1"), 1, "threshold 4: weapon_g3sg1");
	CheckEq(at(7, "weapon_scar20"), 2, "threshold 7: weapon_scar20");
	CheckEq(at(10, "weapon_awp"), 3, "threshold 10: weapon_awp");
	CheckEq(at(12, "weapon_awp"), -1, "threshold 12: weapon_awp is unlimited");
	CheckEq(at(0, "weapon_ak47"), -1001, "weapon_ak47 is not restricted");

	// Threshold selection (GetNiggers-driven best fit).
	CheckEq(rw::BestFitThreshold(config.restrictions, -1), -1, "no threshold below 0 players");
	CheckEq(rw::BestFitThreshold(config.restrictions, 0), 0, "0 players -> threshold 0");
	CheckEq(rw::BestFitThreshold(config.restrictions, 3), 0, "3 players -> threshold 0");
	CheckEq(rw::BestFitThreshold(config.restrictions, 4), 4, "4 players -> threshold 4");
	CheckEq(rw::BestFitThreshold(config.restrictions, 6), 4, "6 players -> threshold 4");
	CheckEq(rw::BestFitThreshold(config.restrictions, 7), 7, "7 players -> threshold 7");
	CheckEq(rw::BestFitThreshold(config.restrictions, 11), 10, "11 players -> threshold 10");
	CheckEq(rw::BestFitThreshold(config.restrictions, 12), 12, "12 players -> threshold 12");
	CheckEq(rw::BestFitThreshold(config.restrictions, 64), 12, "64 players -> threshold 12");

	// Decisions at threshold 10 (limit 3 for weapon_awp).
	const std::string awp = "weapon_awp";
	Check(rw::Evaluate(config, awp, 10, 0).verdict == rw::Verdict::Allowed, "awp 0/3 allowed");
	Check(rw::Evaluate(config, awp, 10, 2).verdict == rw::Verdict::Allowed, "awp 2/3 allowed");
	{
		const rw::Decision decision = rw::Evaluate(config, awp, 10, 3);
		Check(decision.verdict == rw::Verdict::Restricted, "awp 3/3 restricted");
		CheckEq(decision.limit, 3, "restricted limit is reported");
	}
	Check(rw::Evaluate(config, awp, 10, 9).verdict == rw::Verdict::Restricted, "awp 9/3 restricted");

	// Threshold 0 has limit 0: the first weapon is already blocked.
	Check(rw::Evaluate(config, awp, 0, 0).verdict == rw::Verdict::Restricted, "threshold 0 blocks immediately");
	// Limit -1 means "explicitly allowed".
	Check(rw::Evaluate(config, awp, 12, 7).verdict == rw::Verdict::Allowed, "limit -1 is always allowed");
	// Weapons without an entry are never blocked.
	Check(rw::Evaluate(config, "weapon_ak47", 10, 40).verdict == rw::Verdict::Allowed, "unlisted weapon allowed");

	// A map-specific block overrides the "all" block (the original read it second).
	const std::string withMap = R"(
"RestrictedWeapons"
{
	"type_weapons" "2"
	"all"
	{
		"0" { "weapon_awp" "0" }
	}
	"de_dust2"
	{
		"0" { "weapon_awp" "-1" }
	}
}
)";
	rw::Config mapConfig;
	Check(ParseInto(mapConfig, withMap, "de_dust2"), "map config parses");
	Check(rw::Evaluate(mapConfig, awp, 0, 0).verdict == rw::Verdict::Allowed, "map block overrides 'all'");
	rw::Config otherMap;
	Check(ParseInto(otherMap, withMap, "de_mirage"), "other-map config parses");
	Check(rw::Evaluate(otherMap, awp, 0, 0).verdict == rw::Verdict::Restricted, "other maps keep 'all'");

	// Defaults when the file has no settings at all (LoadConfigs' defaults).
	rw::Config empty;
	Check(ParseInto(empty, "\"RestrictedWeapons\" { }", ""), "empty config parses");
	CheckEq(empty.typePlayers, 1, "empty: type_players default");
	CheckEq(empty.typeWeapons, 1, "empty: type_weapons default");
	CheckEq(empty.unblockType, 0, "empty: unblock_type default");
	CheckEq(empty.intervalMessage, 3, "empty: interval_message default");
	Check(empty.specPlayers == false, "empty: spec_players default");
	CheckEqStr(empty.blockSound, "", "empty: block_sound default");
	Check(rw::Evaluate(empty, awp, 10, 10).verdict == rw::Verdict::Allowed, "empty: nothing restricted");

	// Malformed input keeps the previous config (LoadFromBuffer returns false, ParseConfig is not
	// called by the plugin in that case; here the parser itself is checked).
	pisex::KeyValues broken("RestrictedWeapons");
	Check(!broken.LoadFromBuffer("\"RestrictedWeapons\" { \"type_players\" \"2\" "), "malformed config rejected");
}

void TestDefIndexTable() {
	CheckEqStr(rw::WeaponByDefIndex(1), "weapon_deagle", "defindex 1");
	CheckEqStr(rw::WeaponByDefIndex(9), "weapon_awp", "defindex 9");
	CheckEqStr(rw::WeaponByDefIndex(11), "weapon_g3sg1", "defindex 11");
	CheckEqStr(rw::WeaponByDefIndex(31), "weapon_taser", "defindex 31");
	CheckEqStr(rw::WeaponByDefIndex(40), "weapon_ssg08", "defindex 40");
	CheckEqStr(rw::WeaponByDefIndex(60), "weapon_m4a1_silencer", "defindex 60");
	CheckEqStr(rw::WeaponByDefIndex(61), "weapon_usp_silencer", "defindex 61");
	CheckEqStr(rw::WeaponByDefIndex(64), "weapon_revolver", "defindex 64");
	CheckEqStr(rw::WeaponByDefIndex(0), "weapon_knife", "defindex 0 (default branch)");
	CheckEqStr(rw::WeaponByDefIndex(42), "weapon_knife", "unmapped defindex (default branch)");
	CheckEqStr(rw::WeaponByDefIndex(500), "weapon_knife", "unknown defindex (default branch)");
}

void TestHandleValidity() {
	// A real handle: index 93, serial 102949 -> 0xC912805D = -921534371, valid once the game's
	// serial counter has passed 65535 (bit 31 set).
	Check(rw::IsValidHandle(-921534371), "-921534371 (0xC912805D) is a valid handle");
	Check(rw::IsValidHandle(0x7FFFFFFF), "0x7FFFFFFF is a valid handle");
	Check(rw::IsValidHandle(1), "handle 1 is valid");
	Check(!rw::IsValidHandle(0), "0 (the world / \"none\") is not a valid handle");
	Check(!rw::IsValidHandle(-1), "-1 (INVALID_EHANDLE_INDEX) is not a valid handle");
}

} // namespace

int main() {
	TestLiveConfig();
	TestDefIndexTable();
	TestHandleValidity();
	std::printf("rw_core_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
