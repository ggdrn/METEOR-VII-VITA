# FINAL FANTASY VII for PS Vita — v1.3.0

This release adds a cleaner solid-black presentation for 4:3 content and
includes the language-independent PlayStation prompt fix from v1.2.1.

## What's new

- Added **Gameplay > SIDE BORDERS < GRADIENT / BLACK >** to the port menu.
- **BLACK** replaces both gradient side borders with solid black bars.
- The choice applies immediately to every centered 4:3 screen, including game
  menus and movies that remain 4:3 while the 16:9 gameplay option is enabled.
- The setting is saved as `borders_black 0/1` in
  `ux0:data/ffvii-vita/cheats.txt`. Existing installations default to the
  original gradient.
- In-game `(A)`, `(B)`, `(X)` and `(Y)` text labels are converted to
  PlayStation symbols whether or not the source string supplies a color code.
  This applies through the shared field/world-map text path in every language.

## Update / installation

Install **`ff7_vita_v1.3.0.vpk`** with VitaShell over the previous version.
Keep the existing APK, OBB, saves and settings. Back up
`ux0:data/ffvii-vita/save/` before updating.

No game data is included in the VPK. New installations require the user's own
legally purchased Android FINAL FANTASY VII v1.0.29 APK and OBB.

## Validation

- Host compositor tests cover the original gradient and solid-black component
  selection, 4:3 menus/movies and existing field/battle/world presentation.
- Settings tests cover the new default and persistence in `cheats.txt`.
- Button tests cover colored and uncolored labels for all four face buttons.
- The release package is checked for accidental game data.

On-device confirmation is recommended for a 4:3 field, the game menu while
16:9 is selected and a movie. The stock-language Junon prompt fix is completed
in v1.3.1.

FINAL FANTASY VII © SQUARE ENIX. This project is not affiliated with or endorsed
by Square Enix.
