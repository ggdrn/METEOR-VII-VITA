# Name
FINAL FANTASY VII

# Version
1.2.0

# Changelog (Only if update)
- Replaced Gameplay > RANDOM ENCOUNTERS ON/OFF with ENCOUNTER RATE: 0 (OFF), 1 (NORMAL), 2X and 4X.
- Added separate field and world-map encounter calculation multipliers without changing movement speed.
- Preserved world-map Enemy Lure/Away handling and existing 0/1 configuration compatibility. The selected rate is saved automatically.
- The touch button still toggles ON/OFF and resumes the last active multiplier during the session. Button visibility remains independent.
- Added runtime diagnostics for the selected factor, hook calls and original/effective rates.
- 2X/4X multiply calculation parameters, not battle counts. In one deterministic world-map simulation, they produced approximately 1.45x/2.09x as many encounters. On-device frequency validation remains pending.
- Existing limitations: silent snowboard music and possible scene-specific widescreen background/geometry issues.

# Author
Gustavo Garcia (ggdrn)

# AI status
AI-assisted development.

# Description
An unofficial port of Android FINAL FANTASY VII (v1.0.29) to PlayStation Vita. The original ARMv7 game library runs directly on the Vita through a compatibility layer, with vitaGL rendering and native optimizations for fields, battles, the world map and 3D minigames. Features include selectable encounter calculation multipliers (OFF, NORMAL, 2X, 4X), an optional encounter touch button, optional 16:9 widescreen, physical Vita controls, right-stick world-map camera rotation, gameplay settings and optional cheats. Encounter multipliers do not guarantee proportional battle counts. The port reads the original APK and OBB directly. Game data is not included and must come from the user's own legally purchased Android copy.

# Short Description
Android FINAL FANTASY VII port for PS Vita with encounter multipliers and optional widescreen.

# Download Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/download/v1.2.0/ff7_vita_v1.2.0.vpk

# Release Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/tag/v1.2.0

# Sourcecode Link (Optional)
https://github.com/ggdrn/METEOR-VII-VITA

# Data Files Link (Optional)
Not distributed. Supply your own Android v1.0.29 APK and OBB. Installation guide: https://github.com/ggdrn/METEOR-VII-VITA/blob/main/docs/DATA.md

# Requirements (Optional)
- A homebrew-enabled PS Vita.
- kubridge.skprx installed and enabled.
- libshacccg.suprx in ur0:data/ or ur0:data/external/.
- Android FINAL FANTASY VII v1.0.29 game data.
- Approximately 2.1 GB free for game data, plus space for the port.

# Installation
Install ff7_vita_v1.2.0.vpk, then copy base.apk and main.30.com.square_enix.android_googleplay.FFVII.obb intact to ux0:data/ffvii-vita/. Existing users can install over the previous version and keep their data and settings; back up ux0:data/ffvii-vita/save/ before updating.

# Controls
Hold L + R and press Start to open the port menu. Use L/R to switch tabs, Up/Down to select, Left/Right or Cross to change a setting and Circle to close. In Gameplay, select ENCOUNTER RATE for OFF/NORMAL/2X/4X, ENCOUNTERS BUTTON for SHOW/HIDE, or SCREEN for 4:3/16:9.

# Submission note
The download URL above is the intended asset URL. Pushing the Git tag alone does not upload a VPK or submit an update to VitaDB. Attach ff7_vita_v1.2.0.vpk to the matching GitHub Release before using the download link in a submission.
