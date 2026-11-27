# Auto-Attack Forever

![Fantasy adventurers and equipment around a campfire](readme-cover.png)

Starts adaptive melee and ranged combat with the stock Attack action. It switches to a compatible ranged weapon when the target moves out of melee reach and switches back as the target closes. Native attacks still enforce weapon, ammunition, range and facing rules.

## Install

1. Place this repository at `modules/mod-twow-autoattack` in your `tortoise-wow` source tree. Select static linkage with `-DMODULE_MOD_TWOW_AUTOATTACK=static` in your normal server build.
2. Install [the configuration template](conf/mod-twow-autoattack.conf.dist) as `modules/mod-twow-autoattack.conf` beside your active world server configuration. Review its defaults before startup.
3. Back up the affected database. Apply the relevant [SQL files](data/sql) in filename order to the Characters database if you manage module SQL manually. If you use the core module updater, include this module and verify the migrations in the startup log.
4. Deploy the rebuilt world server and start it after completing the database and configuration setup.

## Use

Use `.aaf status`, `.aaf ranged on|off`, and `.aaf melee on|off`. The optional `client-addon/TwowAutoAttack` provides a movable attack button when copied into the client’s `Interface/AddOns` directory.

The character SQL stores individual combat preferences. Based on Auto-Attack-Forever-wotlk by CWO4PapaBear. See the [license](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md).
