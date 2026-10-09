// RestrictedWeapons — Plugify C++ port of the Pisex Metamod:Source plugin
// (ref/src/cs2-restricted_weapons/Core, 406 lines of RestrictedWeapons.cpp + 12/232 of headers).
//
// Behaviour kept from the original:
//   * restricted_weapons.ini is loaded at startup and on every map start (the original's
//     StartupServer ran on every level init): the "all" block and the current map's block,
//     threshold (player count) -> weapon -> allowed count.
//   * The acquiring player's team decides which threshold and which weapons are counted,
//     type_players/type_weapons/spec_players exactly as in GetNiggers()/GetWeaponCount().
//   * CCSPlayer_ItemServices::CanAcquire is hooked (the Pisex utils plugin owned this hook via
//     funchook; here it is our own polyhook detour on the signature from the plugin's
//     gamedata.jsonc - verified unique in the live libserver.so). When a restriction blocks the
//     weapon the engine's own result is replaced with NotAllowedByMode (= 8, what the utils hook
//     returned), the original function is NOT called - same as the utils' SendCanAcquirePre path.
//   * The registered OnWeaponRestricted listeners (IRWApi of the original, used by
//     restricted_weapons_vip) are asked first: returning true unblocks the weapon when
//     unblock_type is 0, or when it is 1 and the weapon's limit is > 0.
//   * The block is announced like the original: at most one message per interval_message seconds
//     per player ("block"/"block_team" phrase + the translated weapon name + the limit) and the
//     block_sound sound event played to that player only.
//
// Porting notes:
//   * The original's item view field pItemView->m_iItemDefinitionIndex() becomes a schema read
//     (CEconItemView::m_iItemDefinitionIndex, 2 bytes) through the s2sdk schema API.
//   * The player slot of the hook comes from s2sdk's PlayerServicesToPlayerSlot (the same
//     service->GetPawn()->GetController()->GetPlayerSlot() chain the Pisex code walked).
//   * The weapon list of a player comes from s2sdk::GetClientWeapons + pisex::GetEntityClassname
//     (the original walked m_pWeaponServices->m_hMyWeapons() and compared GetClassname()).
//   * g_pPlayers->EmitSound(slot, entindex, ...) (single-recipient sound event) maps to
//     s2sdk::EmitSoundToClient with the controller's entity handle.
//
// Fail soft everywhere: without the polyhook/s2sdk bindings, without the signature or without the
// config the plugin logs and keeps running (it just cannot block anything); OnPluginStart never
// returns Failed (that stops every Plugify plugin).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <pisex/pisex.hpp>
#include <pisex/schema.hpp>

#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include <RestrictedWeapons/RestrictedWeapons.hpp> // generated bindings: WeaponRestrictedCallback

#include "rw_core.hpp"

namespace {

constexpr const char* kConfigFile = "restricted_weapons.ini";
constexpr const char* kPhrasesFile = "restricted_weapons.phrases.txt";
constexpr const char* kGamedataSignature = "CCSPlayer_ItemServices_CanAcquire";

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarn(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// --- state -------------------------------------------------------------------------------------

rw::Config g_config;
pisex::PhraseMap g_phrases;						   // weapon display names + block/block_team
std::array<std::time_t, rw::kMaxSlots> g_lastMessage{}; // g_iLastMessage[64] of the original
std::vector<RestrictedWeapons::WeaponRestrictedCallback> g_weaponRestrictedListeners;

polyhook::HookHandle g_canAcquireHook = nullptr;
// The detour target (the address HookDetour was called with): UnhookDetour() keys on THIS, not on
// the trampoline GetOriginalAddr() returns.
void* g_canAcquireAddress = nullptr;
s2sdk::ConfigId g_gamedata = 0; // 0 = none: s2sdk's LoadGameConfigFile answers 0 on failure
int32_t g_defIndexOffset = -1; // CEconItemView::m_iItemDefinitionIndex, -2 = resolution failed

// The phrase of a key, or "" when the key is missing (the original's g_vecPhrases[key] on a
// std::map created an empty entry, so a missing key printed nothing).
const std::string& PhraseText(const std::string& key) {
	static const std::string empty;
	const auto it = g_phrases.find(key);
	return it == g_phrases.end() ? empty : it->second;
}

// --- config ------------------------------------------------------------------------------------

void LoadConfigs(std::string_view mapName) {
	pisex::KeyValues kv("RestrictedWeapons");
	if (!kv.LoadFromFile(kConfigFile)) {
		LogWarn(std::string("RestrictedWeapons: failed to load ") + kConfigFile +
				" - restrictions stay at their previous values");
		return;
	}
	rw::ParseConfig(g_config, kv, mapName);
	LogInfo("RestrictedWeapons: config loaded" + (mapName.empty() ? std::string()
																  : std::string(" (map ") + std::string(mapName) + ")"));
}

void LoadPhrases() {
	// Pisex read the file's "language" key (absent in the shipped file -> the server language);
	// pisexcompat resolves the current language with an "en" fallback, which is the same result.
	pisex::KeyValues kv("Phrases");
	if (!kv.LoadFromFile(kPhrasesFile)) {
		LogWarn(std::string("RestrictedWeapons: failed to load ") + kPhrasesFile);
		return;
	}
	if (const char* language = kv.GetString("language", ""); language && language[0]) {
		pisex::SetPhrasesLanguage(language);
	}
	pisex::LoadPhrasesFile(kPhrasesFile, g_phrases);
}

// --- engine queries (game thread only; CanAcquire runs on it) ----------------------------------

// GetNiggers() of the original: the number of players that count for the acquiring player's team.
int CountPlayers(int iTeam) {
	int iCount = 0;
	for (int i = 0; i < rw::kMaxSlots; ++i) {
		// The original skipped slots without a controller (CCSPlayerController::FromSlot == null)
		// and fake clients.
		if (pisex::GetPlayerController(i) == 0 || pisex::g_pPlayers->IsFakeClient(i)) {
			continue;
		}
		// pisexcompat reads the controller's m_iTeamNum: dead players count, like the original.
		const int iPlayerTeam = pisex::g_pPlayers->GetTeam(i);
		if (iPlayerTeam < 2 && !g_config.specPlayers) {
			continue;
		}
		if (g_config.typePlayers == 1) {
			++iCount;
		} else if (g_config.typePlayers == 2 && iTeam == iPlayerTeam) {
			++iCount;
		}
	}
	return iCount;
}

// Whether a pawn's m_pWeaponServices is attached yet. The schema offset is resolved once and
// never changes; an unknown offset (or a pawn that cannot be resolved) -> true, i.e. the s2sdk
// call decides as before.
bool HasWeaponServices(int iPawn) {
	static const int iOffset = pisex::SchemaOffset("CBasePlayerPawn", "m_pWeaponServices");
	void* const pPawn = pisex::EntIndexToEntPointer(iPawn);
	if (iOffset < 0 || pPawn == nullptr) {
		return true;
	}
	return *reinterpret_cast<void**>(static_cast<char*>(pPawn) + iOffset) != nullptr;
}

// GetWeaponCount() of the original: how many weapons of that classname exist (on the team when
// type_weapons == 2). Fake clients count - the original only skipped a missing pawn there.
int CountWeapons(const std::string& weapon, int iTeam) {
	int iCount = 0;
	for (int i = 0; i < rw::kMaxSlots; ++i) {
		// The original skipped a slot whose controller did not resolve
		// (CCSPlayerController::FromSlot(i) == nullptr). GetPlayerController is the same check
		// without a log line; going straight to GetPlayerPawn would make s2sdk print
		// "Cannot execute 'void* GetClientPawn(int)' on invalid player slot: N" for every
		// unoccupied slot, and this runs on every CanAcquire call.
		if (pisex::GetPlayerController(i) == 0) {
			continue;
		}
		// A pawn exists a moment before the game attaches its weapon services (spawn / respawn /
		// team change); the original only counted a pawn whose m_pWeaponServices was set, and
		// GetClientWeapons would instead log "invalid player service: N" on every CanAcquire
		// call - skip the slot silently.
		const int iPawn = pisex::GetPlayerPawn(i);
		if (iPawn == 0 || !HasWeaponServices(iPawn)) {
			continue;
		}
		if (g_config.typeWeapons == 2 && iTeam != pisex::g_pPlayers->GetTeam(i)) {
			continue;
		}
		for (const int32_t handle : s2sdk::GetClientWeapons(i)) {
			const int index = pisex::EntHandleToEntIndex(handle); // -1/0 when the handle is stale
			if (index <= 0) {
				continue;
			}
			if (pisex::GetEntityClassname(index) == weapon) {
				++iCount;
			}
		}
	}
	return iCount;
}

// pItemView->m_iItemDefinitionIndex() via the schema system (2-byte networked field).
int ReadItemDefinitionIndex(void* itemView) {
	if (g_defIndexOffset == -1) {
		g_defIndexOffset = pisex::SchemaOffset("CEconItemView", "m_iItemDefinitionIndex");
		if (g_defIndexOffset < 0) {
			g_defIndexOffset = -2;
			LogError("RestrictedWeapons: schema field CEconItemView::m_iItemDefinitionIndex not found "
					 "(defindex 0 / weapon_knife is assumed)");
		}
	}
	if (g_defIndexOffset < 0 || !__s2sdk_GetEntData2) {
		return 0;
	}
	return static_cast<int>(static_cast<uint16_t>(s2sdk::GetEntData2(itemView, g_defIndexOffset, 2)));
}

// IRWApi::SendOnWeaponRestrictedCallback of the original: every listener is asked, true wins.
bool SendOnWeaponRestricted(int iSlot, const std::string& weapon) {
	bool bFound = false;
	const plg::string plgWeapon(weapon);
	for (const auto& listener : g_weaponRestrictedListeners) {
		if (listener != nullptr && listener(iSlot, plgWeapon)) {
			bFound = true;
		}
	}
	// TEMPORARY diagnostics of the VIP exemption (capped): listeners asked and their answer.
	static int s_diagBudget = 40;
	if (s_diagBudget > 0) {
		--s_diagBudget;
		LogInfo("[rw-diag] slot " + std::to_string(iSlot) + " " + weapon + " listeners=" +
				std::to_string(g_weaponRestrictedListeners.size()) + " allowed=" + (bFound ? "1" : "0"));
	}
	return bFound;
}

// m_bWarmupPeriod of CCSGameRules: during the warmup restricted weapons are free for everyone.
bool IsWarmupPeriod() {
	if (__s2sdk_GetGameRules == nullptr) {
		return false;
	}
	// The offset is resolved once: pisex::SchemaOffset() hashes the "class\0field" key on every
	// call, and this runs on the CanAcquire path (every buy / pickup attempt; CanAcquire only runs
	// with a live map, so the schema is up). Only the rules *pointer* is fetched per call - it is
	// null between maps.
	static const int offset = pisex::SchemaOffset("CCSGameRules", "m_bWarmupPeriod");
	void* rules = s2sdk::GetGameRules();
	if (rules == nullptr || offset < 0) {
		return false;
	}
	unsigned char value = 0;
	std::memcpy(&value, static_cast<const char*>(rules) + offset, sizeof(value));
	return value != 0;
}

void EmitBlockSound(int iSlot) {
	if (g_config.blockSound.empty() || !__s2sdk_EmitSoundToClient) {
		return;
	}
	const int index = pisex::GetPlayerController(iSlot);
	if (index == 0) {
		return;
	}
	const int handle = pisex::EntIndexToEntHandle(index);
	if (!rw::IsValidHandle(handle)) {
		return;
	}
	// Pisex: EmitSound(iSlot, pController->entindex(), sound, pitch 1, volume 1.0) - a single
	// recipient. s2sdk's EmitSoundToClient takes (volume, pitch).
	s2sdk::EmitSoundToClient(handle, {iSlot}, g_config.blockSound, 1.0f, 1.0f);
}

// --- CanAcquire hook ---------------------------------------------------------------------------

polyhook::ResultType OnCanAcquire(polyhook::HookHandle hook, polyhook::ParametersHandle params,
								  int32_t count, polyhook::ReturnHandle ret, polyhook::CallbackType type) {
	(void)hook;
	(void)count;
	if (type != polyhook::CallbackType::Pre) {
		return polyhook::ResultType::Ignored;
	}
	try {
		void* service = polyhook::GetArgumentPointer(params, 0);	// this: CPlayer_ItemServices*
		void* itemView = polyhook::GetArgumentPointer(params, 1); // CEconItemView*
		if (service == nullptr || itemView == nullptr) {
			return polyhook::ResultType::Ignored;
		}
		if (!__s2sdk_PlayerServicesToPlayerSlot) {
			return polyhook::ResultType::Ignored;
		}
		// service->GetPawn()->GetController()->GetPlayerSlot() of the original.
		const int iSlot = s2sdk::PlayerServicesToPlayerSlot(service);
		if (iSlot < 0 || iSlot >= rw::kMaxSlots) {
			return polyhook::ResultType::Ignored;
		}

		// No threshold block at all (an empty config): nothing can be restricted, so the whole
		// read-and-count below is pointless - BestFitThreshold would answer -1 for every count.
		if (g_config.restrictions.empty()) {
			return polyhook::ResultType::Ignored;
		}
		const std::string weapon = rw::WeaponByDefIndex(ReadItemDefinitionIndex(itemView));
		const int iTeam = pisex::g_pPlayers->GetTeam(iSlot);
		const int iPlayers = CountPlayers(iTeam);
		// The same order as the original: the player count and the weapon's limit are resolved
		// first, and CountWeapons() - which walks every player's weapon list (s2sdk
		// GetClientWeapons + a classname lookup per weapon) - only runs for a weapon this player
		// count actually restricts. An unrestricted weapon (the common case) skips the whole scan.
		const int iLimit = rw::FindLimit(g_config, weapon, iPlayers);
		if (iLimit == rw::kNotRestricted) {
			return polyhook::ResultType::Ignored;
		}
		const int iWeapons = CountWeapons(weapon, iTeam);
		if (iWeapons < iLimit) {
			return polyhook::ResultType::Ignored;
		}
		// Warmup: restricted weapons are free for everyone (the match has not started yet).
		if (IsWarmupPeriod()) {
			return polyhook::ResultType::Ignored;
		}

		// The restriction applies: ask the listeners (VIP) before blocking.
		if (SendOnWeaponRestricted(iSlot, weapon)) {
			if ((g_config.unblockType == 1 && iLimit > 0) || g_config.unblockType == 0) {
				return polyhook::ResultType::Ignored;
			}
		}

		// Announce, at most once per interval_message seconds per player.
		const std::time_t now = std::time(nullptr);
		if (g_lastMessage[static_cast<std::size_t>(iSlot)] + g_config.intervalMessage <= now) {
			g_lastMessage[static_cast<std::size_t>(iSlot)] = now;
			EmitBlockSound(iSlot);
			// The key is hoisted so no temporary is bound to the returned reference (GCC's
			// -Wdangling-reference would flag the ternary's temporary otherwise).
			const std::string blockKey = g_config.typeWeapons == 2 ? "block_team" : "block";
			const std::string& format = PhraseText(blockKey);
			pisex::PrintToChat(iSlot, format.c_str(), PhraseText(weapon).c_str(), iLimit);
		}
		// Skip the original function and hand the engine the result the Pisex utils hook returned.
		polyhook::SetReturnInt32(ret, static_cast<int32_t>(rw::AcquireResult::NotAllowedByMode));
		return polyhook::ResultType::Supercede;
	} catch (const std::exception& ex) {
		LogError(std::string("RestrictedWeapons: exception in the CanAcquire hook: ") + ex.what());
	} catch (...) {
		LogError("RestrictedWeapons: unknown exception in the CanAcquire hook");
	}
	return polyhook::ResultType::Ignored;
}

// Finds the plugin's gamedata.jsonc (the package ships it next to the .so / at the package root,
// like hudmenu's) and returns the CanAcquire function address, 0 on any failure.
void* ResolveCanAcquireAddress(const plg::Plugin& plugin) {
	if (!__s2sdk_LoadGameConfigFile || !__s2sdk_GetGameConfigSignature) {
		LogError("RestrictedWeapons: s2sdk gamedata bindings are missing - the CanAcquire hook is off");
		return nullptr;
	}
	std::vector<std::string> candidates;
	const std::string location(std::string_view(plugin.GetLocation()));
	if (!location.empty()) {
		std::string base = location;
		if (!base.empty() && base.back() == '/') {
			base.pop_back();
		}
		const auto lastSlash = base.find_last_of('/');
		const std::string parent = lastSlash == std::string::npos ? std::string() : base.substr(0, lastSlash);
		const auto parentSlash = parent.find_last_of('/');
		const std::string grandParent = parentSlash == std::string::npos ? std::string() : parent.substr(0, parentSlash);
		candidates.push_back(base + "/gamedata.jsonc");
		if (!parent.empty()) {
			candidates.push_back(parent + "/gamedata.jsonc");
		}
		if (!grandParent.empty()) {
			candidates.push_back(grandParent + "/gamedata.jsonc");
		}
	}
	plg::vector<plg::string> paths;
	for (const std::string& candidate : candidates) {
		if (pisex::FileExists(candidate)) {
			paths.push_back(plg::string(candidate));
		}
	}
	if (paths.empty()) {
		LogError("RestrictedWeapons: gamedata.jsonc not found next to the plugin - the CanAcquire hook is off");
		return nullptr;
	}
	g_gamedata = s2sdk::LoadGameConfigFile(paths);
	if (g_gamedata == 0) {
		LogError("RestrictedWeapons: s2sdk could not load the plugin gamedata - the CanAcquire hook is off");
		return nullptr;
	}
	void* address = s2sdk::GetGameConfigSignature(g_gamedata, plg::string(kGamedataSignature));
	if (address == nullptr) {
		LogError(std::string("RestrictedWeapons: signature ") + kGamedataSignature +
				 " not found in libserver.so - the CanAcquire hook is off");
	}
	return address;
}

void InstallCanAcquireHook(const plg::Plugin& plugin) {
	if (!__polyhook_HookDetour || !__polyhook_AddCallback) {
		LogError("RestrictedWeapons: polyhook bindings are missing (is the polyhook plugin >= 2.5.2 "
				 "installed?) - weapons cannot be restricted");
		return;
	}
	void* address = ResolveCanAcquireAddress(plugin);
	if (address == nullptr) {
		return;
	}
	// AcquireResult::Type CanAcquire(CPlayer_ItemServices* this, CEconItemView* pItemView,
	//                                AcquireMethod::Type eMethod, uint* pLimit)
	const plg::vector<polyhook::DataType> arguments = {
			polyhook::DataType::Pointer, // this
			polyhook::DataType::Pointer, // CEconItemView*
			polyhook::DataType::Int32,	 // AcquireMethod
			polyhook::DataType::Pointer, // uint* pLimit
	};
	try {
		g_canAcquireHook = polyhook::HookDetour(address, polyhook::DataType::Int32, arguments, -1,
												"RestrictedWeapons::CanAcquire");
	} catch (...) {
		g_canAcquireHook = nullptr;
	}
	if (g_canAcquireHook == nullptr) {
		std::string error = "RestrictedWeapons: polyhook HookDetour failed";
		if (__polyhook_GetError) {
			try {
				const plg::string message = polyhook::GetError();
				if (!message.empty()) {
					error += ": ";
					error += std::string_view(message);
				}
			} catch (...) {
			}
		}
		LogError(error);
		return;
	}
	if (!polyhook::AddCallback(g_canAcquireHook, polyhook::CallbackType::Pre, &OnCanAcquire)) {
		LogError("RestrictedWeapons: polyhook AddCallback failed - the CanAcquire hook is off");
		// Nothing of ours is attached: only drop the detour when nobody else is either
		// (UnhookDetour() removes it for every hooker of this address).
		if (__polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour) {
			try {
				if (!polyhook::AreCallbacksRegistered(g_canAcquireHook)) {
					polyhook::UnhookDetour(address);
				}
			} catch (...) {
			}
		}
		g_canAcquireHook = nullptr;
		return;
	}
	g_canAcquireAddress = address;
	LogInfo("RestrictedWeapons: CCSPlayer_ItemServices::CanAcquire hooked");
}

void RemoveCanAcquireHook() {
	if (g_canAcquireHook == nullptr) {
		return;
	}
	try {
		if (__polyhook_RemoveCallback) {
			polyhook::RemoveCallback(g_canAcquireHook, polyhook::CallbackType::Pre, &OnCanAcquire);
		}
		// The trampoline is not a valid unhook key: only UnhookDetour(target) finds the detour,
		// and it removes it for everyone - so wait until no callback is registered on it anymore.
		if (g_canAcquireAddress != nullptr && __polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour &&
			!polyhook::AreCallbacksRegistered(g_canAcquireHook)) {
			polyhook::UnhookDetour(g_canAcquireAddress);
		}
	} catch (...) {
	}
	g_canAcquireHook = nullptr;
	g_canAcquireAddress = nullptr;
}

// --- the plugin --------------------------------------------------------------------------------

class RestrictedWeaponsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			pisex::Init("RestrictedWeapons");
			LoadPhrases();
			// The original's StartupServer ran on every level init (LoadConfigs used the map name
			// of the running map); load once now for a late-loaded plugin and again per map.
			LoadConfigs(pisex::CurrentMap());
			pisex::RegisterMapStartCallback([](const char* map) {
				try {
					LoadConfigs(map == nullptr ? std::string_view() : std::string_view(map));
				} catch (const std::exception& ex) {
					LogError(std::string("RestrictedWeapons: config reload failed: ") + ex.what());
				} catch (...) {
					LogError("RestrictedWeapons: config reload failed with an unknown exception");
				}
			});
			InstallCanAcquireHook(*this);
			LogInfo("RestrictedWeapons: ready");
		} catch (const std::exception& ex) {
			LogError(std::string("RestrictedWeapons: OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("RestrictedWeapons: OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		try {
			pisex::Poll();
		} catch (...) {
			LogError("RestrictedWeapons: Poll() threw");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			RemoveCanAcquireHook();
			// gamedata guide: "Always close gamedata files - Use CloseGameConfigFile() in
			// OnPluginEnd()" (the hook address was resolved already).
			if (g_gamedata != 0 && __s2sdk_CloseGameConfigFile) {
				s2sdk::CloseGameConfigFile(g_gamedata);
				g_gamedata = 0;
			}
			pisex::Shutdown();
		} catch (const std::exception& ex) {
			LogError(std::string("RestrictedWeapons: OnPluginEnd failed: ") + ex.what());
		} catch (...) {
			LogError("RestrictedWeapons: OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

RestrictedWeaponsPlugin g_restrictedWeaponsPlugin;

// --- exported IRWApi (the restricted_weapons_vip module registers here) -------------------------
// The original keyed its listeners by PluginId, one callback per module; every registered plugin
// is asked (the original kept them in a vector and OR'ed the results).

template <typename Callback>
void AddListener(std::vector<Callback>& list, Callback callback) {
	if (callback == nullptr) {
		return;
	}
	if (std::find(list.begin(), list.end(), callback) == list.end()) {
		list.push_back(callback);
	}
}

template <typename Callback>
void RemoveListener(std::vector<Callback>& list, Callback callback) {
	list.erase(std::remove(list.begin(), list.end(), callback), list.end());
}

} // namespace

extern "C" PLUGIN_API void OnWeaponRestricted_Register(RestrictedWeapons::WeaponRestrictedCallback callback) {
	try {
		AddListener(g_weaponRestrictedListeners, callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnWeaponRestricted_Unregister(RestrictedWeapons::WeaponRestrictedCallback callback) {
	try {
		RemoveListener(g_weaponRestrictedListeners, callback);
	} catch (...) {
	}
}

PLUGIFY_PLUGIN(PLUGIN_API, &g_restrictedWeaponsPlugin)
