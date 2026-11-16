# FINAL FANTASY VII for PS Vita — v1.3.1

This release adds a solid-black side-border option and fixes PlayStation button
prompts that were missing when using the original Android game data without a
modified field archive.

## What's fixed

- Stock Android field files whose dialog table follows the stored script length
  are now accepted, so their button prompts are converted correctly.
- Stock `[A]`, `[B]`, `[X]` and `[Y]` labels are mapped to the corresponding
  PlayStation glyphs. The existing colored and uncolored `(A)/(B)/(X)/(Y)`
  conversion remains available for translated or modified data.
- The final field-text rendering path also performs the conversion after the
  selected language text is ready, covering prompts that were previously not
  visible in unmodified game data.

## Included from v1.3.0

- **Gameplay > SIDE BORDERS < GRADIENT / BLACK >** switches every centered
  4:3 presentation, including menus and movies, to the selected side borders.
- The choice is saved as `borders_black 0/1` in `cheats.txt`.

## Update / installation

Install **`ff7_vita_v1.3.1.vpk`** with VitaShell over the previous version.
Keep the existing APK, OBB, saves and settings. No game data is included.

## Validation

- Host tests cover stock and modified field layouts, all four stock labels,
  original button opcodes, colored labels and uncolored labels.
- Existing compositor and settings tests cover the side-border option.
- The release package is checked for accidental game data.

Please confirm on device in the Junon parade sequence. The log should now show
at least one `ff7_buttons: field text: N button labels -> PlayStation symbols`
line; zero-result fields are also printed.

FINAL FANTASY VII © SQUARE ENIX. This project is not affiliated with or endorsed
by Square Enix.
