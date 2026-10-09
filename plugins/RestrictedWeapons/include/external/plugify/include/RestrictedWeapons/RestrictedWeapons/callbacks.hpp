#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from RestrictedWeapons.pplugin (group: callbacks)

namespace RestrictedWeapons {

  using _OnWeaponRestricted_Register = void (*)(WeaponRestrictedCallback);
}
extern "C" PLUGIN_API RestrictedWeapons::_OnWeaponRestricted_Register __RestrictedWeapons_OnWeaponRestricted_Register;
namespace RestrictedWeapons {
  /**
   * @brief Registers the callback the plugin runs when a restriction would block a weapon the player is about to pick up or buy - the IRWApi::HookOnWeaponRestricted of the original (there keyed by PluginId, so one listener per module; registering the same function pointer again replaces/keeps a single entry, and every registered plugin is asked). Return true from the callback to let the weapon through. The restricted_weapons_vip module is the original user of this hook.
   * @param callback (function)
   */
  inline void OnWeaponRestricted_Register(WeaponRestrictedCallback callback) {
    return __RestrictedWeapons_OnWeaponRestricted_Register(callback);
  }

  using _OnWeaponRestricted_Unregister = void (*)(WeaponRestrictedCallback);
}
extern "C" PLUGIN_API RestrictedWeapons::_OnWeaponRestricted_Unregister __RestrictedWeapons_OnWeaponRestricted_Unregister;
namespace RestrictedWeapons {
  /**
   * @brief Removes the OnWeaponRestricted listener registered by this plugin. Idempotent.
   * @param callback (function)
   */
  inline void OnWeaponRestricted_Unregister(WeaponRestrictedCallback callback) {
    return __RestrictedWeapons_OnWeaponRestricted_Unregister(callback);
  }

} // namespace RestrictedWeapons
