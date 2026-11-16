# FINAL FANTASY VII for PS Vita — v1.2.1

This maintenance release fixes PlayStation button symbols in game text across
all supported languages.

## What's fixed

- In-game labels written as uncolored `(A)`, `(B)`, `(X)` and `(Y)` are now
  converted to the matching PlayStation symbols, in addition to the colored
  label format already handled by the port.
- The fix is applied by the central field/world-map string converter, so it is
  not tied to Junon or to a specific translation.
- Existing colored prompts and the Android touch-button atlas remain supported.
- The physical control mapping is unchanged: Cross confirms, Circle cancels,
  Square and Triangle retain their original actions.

## Update / installation

Install **`ff7_vita_v1.2.1.vpk`** with VitaShell over the previous version.
Keep the existing `base.apk`, OBB and settings. Back up
`ux0:data/ffvii-vita/save/` before updating.

No game data is included in the VPK. New installations still require the
user's legally purchased Android FINAL FANTASY VII v1.0.29 APK and OBB.

## Validation

- Host tests cover colored and uncolored labels for all four face buttons,
  unrelated parenthesized text, and malformed/non-terminated input.
- The VPK package check verifies that no APK, OBB, save or other game data is
  included.
- On-device confirmation is still recommended for the Junon parade sequence
  and another prompt in a different language.

FINAL FANTASY VII © SQUARE ENIX. This project is not affiliated with or endorsed
by Square Enix.
