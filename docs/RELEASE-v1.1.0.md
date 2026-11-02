# FINAL FANTASY VII for PS Vita — v1.1.0

This update adds direct control over random encounters and an option to hide the
Android encounter button for a cleaner screen.

## What's new

- **Gameplay > RANDOM ENCOUNTERS: ON/OFF** controls random encounters directly
  from the port menu, including while the on-screen button is hidden.
- **Gameplay > ENCOUNTERS BUTTON: SHOW/HIDE** shows or hides the touch button.
  Hiding it also removes its touch target and leaves the encounter setting unchanged.
- The menu and on-screen button share the same encounter state. Changes apply
  immediately and are saved in `ux0:data/ffvii-vita/cheats.txt`, including encounter
  changes made by touching the visible button.
- Existing configurations default to encounters **ON** and button **SHOW**.

## Update / installation

Install **`ff7_vita_v1.1.0.vpk`** with VitaShell over the previous version. Keep
existing game data and settings; back up `ux0:data/ffvii-vita/save/` before updating.
No replacement APK or OBB is needed for this update.

For a new installation, the requirements remain the same: a homebrew-enabled
PS Vita, `kubridge.skprx`, `libshacccg.suprx`, and the user's own Android FINAL
FANTASY VII **v1.0.29** game data. Copy the original files intact to:

```text
ux0:data/ffvii-vita/base.apk
ux0:data/ffvii-vita/main.30.com.square_enix.android_googleplay.FFVII.obb
```

See [the installation guide](https://github.com/ggdrn/METEOR-VII-VITA/blob/main/docs/DATA.md)
for the complete setup instructions.

## Controls

Hold **L + R** and press **Start** to open the port menu, then select **Gameplay**
with **L/R**. Use **Up/Down** to select, **Left/Right** or **Cross** to change a
setting, and **Circle** to close. No restart is needed for either encounter option.

## Known limitations

- Snowboard music remains silent.
- Some scenes may expose background or geometry limitations in widescreen;
  switch back to 4:3 if a scene does not display correctly.
- Automated tests and the release build passed. On-device validation of the new
  options remains pending; see [the validation record](RELEASE-v1.1.0-VALIDATION.md).

For bug reports, include the port version, location, selected screen mode, steps
to reproduce and `ux0:data/ffvii-vita/logs.txt` plus `logs_anterior.txt`.

## Credits

Port by **Gustavo Garcia (ggdrn)**, with AI-assisted development.
Thanks to the contributors behind soloader-boilerplate, so-util, FalsoJNI,
vitaGL, VitaSDK, kubridge and the Vita homebrew community.

The VPK contains the port only. Game data must be supplied by the user.
FINAL FANTASY VII © SQUARE ENIX. This project is not affiliated with or endorsed
by Square Enix.
