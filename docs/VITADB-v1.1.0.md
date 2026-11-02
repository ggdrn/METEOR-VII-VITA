# Name
FINAL FANTASY VII

# Version
1.1.0

# Changelog (Only if update)
- Added Gameplay > RANDOM ENCOUNTERS: ON/OFF to control random encounters directly from the port menu.
- Added Gameplay > ENCOUNTERS BUTTON: SHOW/HIDE to hide the Android touch button for a cleaner screen.
- Hidden encounter buttons no longer accept touches. Button visibility does not change the encounter setting.
- The menu and on-screen button stay synchronized; both choices are saved, including encounter changes made through touch.
- Existing configurations default to encounters ON and button SHOW.
- Known limitations: silent snowboard music; some scenes may show background or geometry limitations in widescreen. On-device validation of the new encounter options remains pending.

# Author
Gustavo Garcia (ggdrn)

# AI status
AI-assisted development.

# Description
An unofficial port of the Android version of FINAL FANTASY VII (v1.0.29) to PlayStation Vita. The original ARMv7 game library runs directly on the Vita through a compatibility layer, with vitaGL rendering and native optimizations for fields, battles, the world map and 3D minigames. Features include direct menu control of random encounters, an optional encounter touch button, optional 16:9 widescreen, physical Vita controls, right-stick world map camera rotation, gameplay settings and optional cheats. The port reads the original APK and OBB directly. Game data is not included and must come from the user's own legally purchased Android copy.

# Short Description
Android FINAL FANTASY VII port for PS Vita with optional widescreen.

# Download Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/download/v1.1.0/ff7_vita_v1.1.0.vpk

# Release Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/tag/v1.1.0

# Sourcecode Link (Optional)
https://github.com/ggdrn/METEOR-VII-VITA

# Data Files Link (Optional)
Not distributed. Supply your own Android v1.0.29 APK and OBB. Installation guide: https://github.com/ggdrn/METEOR-VII-VITA/blob/HEAD/docs/DATA.md

# Requirements (Optional)
- A homebrew-enabled PS Vita.
- kubridge.skprx installed and enabled.
- libshacccg.suprx in ur0:data/ or ur0:data/external/.
- Android FINAL FANTASY VII v1.0.29 game data.
- Approximately 2.1 GB free for game data, plus space for the port.

# Installation
Install ff7_vita_v1.1.0.vpk, then copy base.apk and main.30.com.square_enix.android_googleplay.FFVII.obb to ux0:data/ffvii-vita/. Keep both files intact. Existing users can install over the previous version; back up ux0:data/ffvii-vita/save/ before updating.

# Controls
Hold L + R and press Start to open the port menu. Use L/R to switch tabs, Up/Down to select, Left/Right to change, Cross to confirm and Circle to close. In Gameplay, select RANDOM ENCOUNTERS to turn encounters ON/OFF, ENCOUNTERS BUTTON to SHOW/HIDE the touch button, or SCREEN to switch between 4:3 and 16:9.
