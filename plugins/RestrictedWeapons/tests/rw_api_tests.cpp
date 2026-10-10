// Tests for plugins/RestrictedWeapons/include/rw_api.hpp - the consumer side of the
// RestrictedWeapons API (what the ported restricted_weapons_vip module compiles against).
//
// The generated binding slots (__RestrictedWeapons_*) are filled here with stubs, so the whole
// wrapper is exercised without a live plugin: the plg::string <-> const char* conversion, the
// registration/unregistration pair and the static trampoline that dispatches the std::function
// callback (captures included) and swallows exceptions.
//
// It also checks the "plugin not loaded" behaviour: with null binding slots every wrapper call has
// to be a harmless no-op instead of dereferencing a null function pointer.

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <plg/plugin.hpp>

#include <rw_api.hpp>

// A real plugin defines these through PLUGIFY_PLUGIN; the consumer header only needs plg::Log
// (null = logging stays off, the state the wrapper must survive).
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

// --- the fake server side --------------------------------------------------------------------

int g_registerCalls = 0;
int g_unregisterCalls = 0;
RestrictedWeapons::WeaponRestrictedCallback g_registered = nullptr;

void StubRegister(RestrictedWeapons::WeaponRestrictedCallback callback) {
	++g_registerCalls;
	g_registered = callback;
}

void StubUnregister(RestrictedWeapons::WeaponRestrictedCallback callback) {
	++g_unregisterCalls;
	if (g_registered == callback) {
		g_registered = nullptr;
	}
}

void InstallStubs() {
	__RestrictedWeapons_OnWeaponRestricted_Register = &StubRegister;
	__RestrictedWeapons_OnWeaponRestricted_Unregister = &StubUnregister;
}

void ClearStubs() {
	__RestrictedWeapons_OnWeaponRestricted_Register = nullptr;
	__RestrictedWeapons_OnWeaponRestricted_Unregister = nullptr;
	g_registered = nullptr;
}

} // namespace

int main() {
	ClearStubs();
	restricted_weapons::RWApi& api = restricted_weapons::RWApi::Get();

	// 1. Plugin not loaded: registration is a no-op, nothing crashes.
	api.HookOnWeaponRestricted([](int, const char*) { return true; });
	Check(g_registerCalls == 0, "no registration while the plugin is not loaded");
	api.UnhookOnWeaponRestricted();
	Check(g_unregisterCalls == 0, "no unregistration while the plugin is not loaded");

	// 2. Plugin loaded: the callback is registered through the trampoline.
	InstallStubs();
	int seenSlot = -1;
	std::string seenWeapon;
	api.HookOnWeaponRestricted([&](int iSlot, const char* szWeapon) {
		seenSlot = iSlot;
		seenWeapon = szWeapon;
		return szWeapon != nullptr && std::string(szWeapon) == "weapon_awp";
	});
	CheckEq(g_registerCalls, 1, "register called once");
	Check(g_registered != nullptr, "the server got the trampoline");
	Check(g_registered != nullptr && g_registered(7, plg::string("weapon_awp")), "trampoline returns the callback result");
	CheckEq(seenSlot, 7, "slot passed through");
	Check(seenWeapon == "weapon_awp", "weapon passed through as const char*");
	Check(g_registered != nullptr && !g_registered(7, plg::string("weapon_ak47")), "trampoline forwards 'false'");

	// 3. Re-registering replaces the previous callback (one listener per plugin, like the original).
	InstallStubs();
	g_registerCalls = 0;
	api.HookOnWeaponRestricted(nullptr);
	CheckEq(g_registerCalls, 1, "re-registration still goes to the server");
	Check(g_registered != nullptr && !g_registered(3, plg::string("weapon_awp")), "a null callback answers false");
	Check(g_registered != nullptr && !g_registered(3, plg::string("weapon_awp")), "and repeatably so");

	// 4. A throwing callback must not unwind into the engine.
	struct Boom : std::runtime_error {
		Boom() : std::runtime_error("boom") {}
	};
	api.HookOnWeaponRestricted([](int, const char*) -> bool { throw Boom(); });
	Check(g_registered != nullptr, "the throwing callback is still registered");
	Check(g_registered != nullptr && !g_registered(1, plg::string("weapon_awp")), "exception answers false");

	// 5. Unregister removes this plugin's listener and drops the callback.
	InstallStubs();
	g_unregisterCalls = 0;
	api.UnhookOnWeaponRestricted();
	CheckEq(g_unregisterCalls, 1, "unregister called once");
	Check(g_registered == nullptr, "the server dropped the trampoline");
	api.UnhookOnWeaponRestricted(); // idempotent
	CheckEq(g_unregisterCalls, 2, "unregister is idempotent on our side");

	ClearStubs();
	std::printf("rw_api_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
