#pragma once
// Consumer-side wrapper for the RestrictedWeapons plugin API (the IRWApi of the Pisex original,
// ref/src/cs2-restricted_weapons/Core/include/restricted_weapons.h).
//
// The generated headers (RestrictedWeapons/RestrictedWeapons.hpp and friends, produced by
// bin/plugify-gen from RestrictedWeapons.pplugin) expose the raw boundary: plain C function
// pointers and plg::string values. This header restores the shape the Pisex module was written
// against:
//
//   * a std::function callback with captures, dispatched from a static trampoline;
//   * const char* arguments, returning bool;
//   * the same method name: HookOnWeaponRestricted().
//
// Porting restricted_weapons_vip is then mechanical - keep `g_pRWApi` and make it point at
// restricted_weapons::RWApi::Get():
//
//   #include <rw_api.hpp>                        // -I plugins/RestrictedWeapons/include
//   #include <RestrictedWeapons/RestrictedWeapons.hpp>  // -I .../include/external/plugify/include
//
//   restricted_weapons::RWApi* g_pRWApi = &restricted_weapons::RWApi::Get();
//   g_pRWApi->HookOnWeaponRestricted([](int iSlot, const char* szWeapon) -> bool { ... });
//
// Semantics kept from the original: the callback returns true when *this* plugin unblocks the
// weapon; the server ORs the results of all registered plugins (the original kept one
// std::function per module in a vector and did the same). One registration per plugin - calling
// HookOnWeaponRestricted again replaces the previous callback of this plugin.
//
// Every trampoline swallows exceptions (the boundary must not unwind into the engine) and logs
// them through plg::Log. When the RestrictedWeapons plugin is not loaded the calls are no-ops.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <RestrictedWeapons/RestrictedWeapons.hpp>

namespace restricted_weapons {

// OnWeaponRestrictedCallback of the original IRWApi (std::function<bool(int, const char*)>).
using OnWeaponRestrictedCallback = std::function<bool(int iSlot, const char* szWeapon)>;

namespace detail {

inline void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// True when the plugify module filled in the binding slots, i.e. when RestrictedWeapons is loaded.
inline bool Available() {
	return __RestrictedWeapons_OnWeaponRestricted_Register != nullptr;
}

inline std::string ToStd(const plg::string& value) {
	return std::string(std::string_view(value));
}

// The single callback this plugin registered (the original stored the module's std::function).
inline OnWeaponRestrictedCallback& Callback() {
	static OnWeaponRestrictedCallback callback;
	return callback;
}

// The static trampoline handed to the server; its address is the plugin's identity there.
inline bool WeaponRestrictedTrampoline(int32_t slot, const plg::string& weapon) {
	try {
		const OnWeaponRestrictedCallback& callback = Callback();
		if (!callback) {
			return false;
		}
		const std::string weaponCopy = ToStd(weapon); // the original passed a const char*
		return callback(slot, weaponCopy.c_str());
	} catch (...) {
		LogError("RestrictedWeapons: OnWeaponRestricted callback threw");
		return false;
	}
}

} // namespace detail

// One instance per plugin; named RWApi like the original's interface, with the original method.
class RWApi {
public:
	static RWApi& Get() {
		static RWApi api;
		return api;
	}

	RWApi(const RWApi&) = delete;
	RWApi& operator=(const RWApi&) = delete;

	// IRWApi::HookOnWeaponRestricted(OnWeaponRestrictedCallback).
	void HookOnWeaponRestricted(OnWeaponRestrictedCallback callback) {
		detail::Callback() = std::move(callback);
		if (detail::Available()) {
			::RestrictedWeapons::OnWeaponRestricted_Register(&detail::WeaponRestrictedTrampoline);
		}
	}

	// Drops the callback and unregisters from the server (idempotent). The original had no
	// unregister (listeners lived until the module unloaded); this is the natural plugify pair.
	void UnhookOnWeaponRestricted() {
		detail::Callback() = nullptr;
		if (detail::Available() && __RestrictedWeapons_OnWeaponRestricted_Unregister != nullptr) {
			::RestrictedWeapons::OnWeaponRestricted_Unregister(&detail::WeaponRestrictedTrampoline);
		}
	}

private:
	RWApi() = default;
};

// The object the ported module uses; name it g_pRWApi to keep its code unchanged:
//
//     restricted_weapons::RWApi* g_pRWApi = &restricted_weapons::RWApi::Get();
inline RWApi& RW() {
	return RWApi::Get();
}

} // namespace restricted_weapons
