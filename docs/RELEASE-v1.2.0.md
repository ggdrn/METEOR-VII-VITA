# FINAL FANTASY VII for PS Vita — v1.2.0

This update adds selectable encounter calculation multipliers to the port menu,
with separate field and world-map handling, saved settings and runtime diagnostics.

## What's new

- **Gameplay > ENCOUNTER RATE** replaces the ON/OFF option with **0 (OFF)**,
  **1 (NORMAL)**, **2X** and **4X**. Changes apply without restarting the game.
- Field encounters scale their probability parameter during the original check,
  then restore it. World-map encounters scale the original effective rate after
  the game's Enemy Lure/Away calculation. Movement speed is unchanged.
- The selected setting is saved in `ux0:data/ffvii-vita/cheats.txt`. Existing
  `encounters 0` and `encounters 1` configurations remain compatible; the default
  is NORMAL.
- The optional Android touch button still shows ON/OFF. Turning encounters back
  on resumes the last active multiplier during the current session. Restarting
  with OFF saved resets the next activation to NORMAL.
- **ENCOUNTERS BUTTON: SHOW/HIDE** remains independent of the encounter rate.
- Runtime logs now include the selected factor, field/world hook call counts
  and the original/effective rates, making device reports easier to verify.
- If multiplier hooks are unavailable for an unsupported library layout, the
  menu falls back to OFF/NORMAL.

## How 2X and 4X behave

**These labels multiply the game's encounter calculation parameters, not the
number of battles per minute or per distance traveled.** The original game
accumulates danger and uses random checks, so battle frequency is not linear.

A deterministic ARM simulation of 10,000 world-map checks on the same terrain,
modeling a battle return by resetting danger, produced:

| Setting | Encounters | Relative to NORMAL |
| --- | ---: | ---: |
| 1 (NORMAL) | 1,005 | 1.00× |
| 2X | 1,459 | 1.45× |
| 4X | 2,096 | 2.09× |

These are simulation results for one setup, not guarantees for other areas or
hardware measurements. A first device log confirmed hook installation, but did
not record enough information to verify the active rate during walking.
Confirmation of the frequency changes on Vita remains pending.

## Update / installation

Install **`ff7_vita_v1.2.0.vpk`** with VitaShell over the previous version.
Keep existing game data and settings; back up `ux0:data/ffvii-vita/save/` before
updating. No replacement APK or OBB is needed.

New installations require a homebrew-enabled PS Vita, `kubridge.skprx`,
`libshacccg.suprx`, and the user's own Android FINAL FANTASY VII **v1.0.29**
data, copied intact to:

```text
ux0:data/ffvii-vita/base.apk
ux0:data/ffvii-vita/main.30.com.square_enix.android_googleplay.FFVII.obb
```

See [the installation guide](https://github.com/ggdrn/METEOR-VII-VITA/blob/main/docs/DATA.md).

## Controls

Hold **L + R** and press **Start** to open the port menu. Select **Gameplay**
with **L/R**, select **ENCOUNTER RATE** with **Up/Down**, then use **Left/Right**
or **Cross** to cycle settings. Press **Circle** to close.

## Validation and known limitations

- Host tests cover rate selection, settings compatibility and persistence,
  touch OFF/ON behavior, independent button visibility, field rate restoration
  and world-map multiplication.
- ARM tests verify the original field probability comparison, Enemy Lure/Away
  rates and the world-map danger accumulation/random comparison.
- Release build and package checks passed; the VPK contains no game data.
- Multipliers retain the original encounter rules. On-device frequency and
  transition validation remain pending.
- Snowboard music remains silent. Some scenes may expose background or geometry
  limitations in widescreen; switch back to 4:3 if needed.

See [the validation record](RELEASE-v1.2.0-VALIDATION.md). For bug reports, include
the location, selected encounter rate, steps to reproduce, and
`ux0:data/ffvii-vita/logs.txt` plus `logs_anterior.txt`. To test rate handling,
walk in the same area with NORMAL and 4X before collecting the logs.

## Credits

Port by **Gustavo Garcia (ggdrn)**, with AI-assisted development.
Thanks to the contributors behind soloader-boilerplate, so-util, FalsoJNI,
vitaGL, VitaSDK, kubridge and the Vita homebrew community.

The VPK contains the port only. Game data must be supplied by the user.
FINAL FANTASY VII © SQUARE ENIX. This project is not affiliated with or endorsed
by Square Enix.
