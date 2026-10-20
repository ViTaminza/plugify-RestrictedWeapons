# RestrictedWeapons (Plugify)

Limits how many players of a team (or of the whole server, depending on the config) may carry a weapon: when the limit of the weapon is exhausted, the next pickup or buy is refused and announced in chat, with the block sound of the config. The limit that applies is the highest 'player count' threshold of restricted_weapons.ini that is <= the current player count. A port of the Metamod:Source plugin 'RestrictedWeapons' by Pisex; the config file restricted_weapons.ini and the translations keep the original layout. Also exports IRWApi (OnWeaponRestricted) for the restricted_weapons_vip module.

Plugify port (fork) of **RestrictedWeapons** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-restricted_weapons
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, polyhook, hudmenu (optional)

## Installation
Download `RestrictedWeapons-1.0.2.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/RestrictedWeapons`.

## License
GPL-3.0, see LICENSE.

