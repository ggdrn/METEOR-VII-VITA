# Name
FINAL FANTASY VII

# Version
1.0

# Changelog (Only if update)
- Added optional 16:9 widescreen for fields, battles and the world map; 4:3 remains available in the port menu.
- Adjusted field backgrounds, world map sky and clouds, battle viewports, HUD alignment and dialog placement for widescreen.
- Battle entry transitions fill the display when widescreen is selected.
- Includes native rendering optimizations, GPU palette rendering, texture caching and minigame performance improvements.
- Includes gameplay settings, optional cheats, dialog language selection and right-stick world map camera controls.
- Includes previous fixes for PlayStation button prompts, menu audio, movie presentation and minigame inputs.
- Developer tools are hidden from the standard port menu; the public tabs are Cheats and Gameplay.
- Known limitations: silent snowboard music; some scenes may show background or geometry limitations in widescreen.

# Author
Gustavo Garcia (ggdrn)

# AI status
AI-assisted development.

# Description
An unofficial port of the Android version of FINAL FANTASY VII (v1.0.29) to PlayStation Vita. The original ARMv7 game library runs directly on the Vita through a compatibility layer, with vitaGL rendering and native optimizations for fields, battles, the world map and 3D minigames. Features include optional 16:9 widescreen, physical Vita controls, right-stick world map camera rotation, gameplay settings and optional cheats. The port reads the original APK and OBB directly. Game data is not included and must come from the user's own legally purchased Android copy.

# Short Description
Android FINAL FANTASY VII port for PS Vita with optional widescreen.

# Download Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/download/v1.0/ff7_vita_v1.0.vpk

# Release Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/tag/v1.0

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
Install ff7_vita_v1.0.vpk, then copy base.apk and main.30.com.square_enix.android_googleplay.FFVII.obb to ux0:data/ffvii-vita/. Keep both files intact. Existing users can install over the previous version; back up ux0:data/ffvii-vita/save/ before updating.

# Controls
Hold L + R and press Start to open the port menu. Use L/R to switch tabs, Up/Down to select, Left/Right to change, Cross to confirm and Circle to close. Select Gameplay > SCREEN to switch between 4:3 and 16:9.
