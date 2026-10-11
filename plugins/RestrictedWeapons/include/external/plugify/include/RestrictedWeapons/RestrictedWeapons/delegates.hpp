#pragma once

#include "enums.hpp"
#include "aliases.hpp"
// Generated from RestrictedWeapons.pplugin

namespace RestrictedWeapons {

  // Called when a restricted weapon is about to be refused; return true to unblock it for this player.
  using WeaponRestrictedCallback = bool (*)(int32_t, const plg::string&);


} // namespace RestrictedWeapons
