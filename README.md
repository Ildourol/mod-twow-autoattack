# Auto-Attack Forever for Turtle WoW

Auto-Attack Forever is a server module for Turtle WoW's custom vMaNGOS fork, `tortoise-wow`. Right-click a hostile target or use the stock Attack action to start combat. The module chooses ranged immediately when the target is outside melee reach and a compatible ranged weapon is available. It switches to melee when the target closes and resumes ranged attacks when the distance allows. Supported learned ranged abilities can also start adaptive combat.

Hunter Auto Shot and wand Shoot use native autorepeat. Learned Shoot Bow, Shoot Gun, Shoot Crossbow, and Throw use native single casts paced by the ranged weapon timer. The core enforces range, facing, movement, ammunition, weapon restrictions, and cast times.

The port targets `tortoise-wow` revision `a4972d9` and Turtle WoW client `1.18.1.7272`.

## Install

1. Place this repository at `modules/mod-twow-autoattack` in your `tortoise-wow` source tree.
2. Configure the server build with `-DMODULE_MOD_TWOW_AUTOATTACK=static`, then build and install `mangosd`.
3. Copy `conf/mod-twow-autoattack.conf.dist` to `modules/mod-twow-autoattack.conf` under the directory containing your active `mangosd.conf`.
4. Apply the two SQL files in `data/sql/character/` to the **character database** in numeric order. They store per-character preferences. Until the tables are available, the module uses the server defaults.
5. Review the module config and restart. The supplied template enables the feature.

The stock client works with its existing controls and data. To add a movable attack button that displays the equipped ranged weapon's icon, copy `client-addon/TwowAutoAttack` into the client's `Interface/AddOns` directory. The stock action bar may show no autorepeat glow for a ranged attack started by the server.

## Player commands

| Command | Effect |
| --- | --- |
| `.aaf status` | Show your ranged and close-range settings. |
| `.aaf ranged on` or `.aaf ranged off` | Enable or disable adaptive ranged attacks for your character. |
| `.aaf melee on` or `.aaf melee off` | Prefer melee at close range or continue shooting where the spell's native minimum range allows. |

`.rangedauto` is an alias for `.aaf`. The stock Stop Attack action stops combat. By default, changing or clearing the selected target also stops a managed ranged attack.

## Server settings

| Key | Default | Effect |
| --- | --- | --- |
| `TwowAutoAttack.Enable` | `1` | Enable adaptive attacks. |
| `TwowAutoAttack.RightClick` | `1` | Enable adaptive combat from hostile right-click and the stock Attack action. |
| `TwowAutoAttack.CloseRange.Melee` | `1` | Prefer melee at close range for characters without an override. |
| `TwowAutoAttack.StopOnTargetChange` | `1` | Stop managed ranged attacks when the selected target changes or clears. |
| `TwowAutoAttack.Announce` | `0` | Show the settings command at login. |
| `TwowAutoAttack.Debug` | `0` | Log mode changes, stop reasons, and native cast result codes. |
| `TwowAutoAttack.Debug.PlayerGuid` | `0` | Trace all eligible players, or set a positive character GUID to trace one. |

Settings take effect at server startup. Debug messages use the `TwowAutoAttack:` prefix.

## Disable or remove

Set `TwowAutoAttack.Enable = 0` and restart to disable the feature. To uninstall, disable the module in CMake and rebuild, then remove the module config and optional add-on. Keep the preference tables if you plan to reinstall. To remove the saved preferences permanently, back up the character database and drop only `mod_twow_autoattack_ranged_disabled` and `mod_twow_autoattack_melee_preference`.

## License and credits

Licensed under [GPL-2.0-or-later](LICENSE). Based on Auto-Attack-Forever-wotlk by CWO4PapaBear. See [attribution](THIRD_PARTY_NOTICES.md).
