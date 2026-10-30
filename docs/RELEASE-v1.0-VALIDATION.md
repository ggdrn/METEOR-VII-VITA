# v1.0 release validation

## Prepared artifacts

- GitHub release text: `docs/RELEASE-v1.0.md`.
- VitaDB submission text: `docs/VITADB-v1.0.md`.
- Release tag: `v1.0` (already published; see source status below).
- VPK: `build/VPK/ff7_vita_v1.0.vpk`.
- SHA-256 file: `build/VPK/ff7_vita_v1.0.vpk.sha256`.
- VPK size: 3,101,494 bytes.
- SHA-256: `02f55887c4303cbaf84e4ed4f3ced454267c3e8d209adfb75b2ac476584d28cb`.
- Application version: `01.00`; title ID: `FFSQ00007`.

## Completed checks

The v1.0 VPK was rebuilt after hiding DEBUG from the default menu. The
developer marker is absent from the package. Both drawing and L/R navigation
use the restricted tab count until the marker is present at startup.
The widescreen regression results below are from the preceding build; its
rendering code is unchanged in this rebuild.

- Release build with self-tests, continuous profiling and spike profiling disabled.
- VPK content allow-list and LiveArea image validation passed.
- ZIP integrity and embedded application version/title ID passed.
- No APK, OBB, native game library or game progress files in the VPK.
- `python3 scripts/test_screen.py` passed.
- `python3 scripts/test_wide.py` passed.
- `python3 scripts/emu/test_wide_cloud.py` passed.
- `python3 scripts/emu/test_wide_field2.py` passed.
- Unicorn tests required JIT execution outside the macOS sandbox.

## Publication and device validation

These are local release artifacts. No GitHub release has been created and no
VitaDB submission has been sent by this preparation step. The download and
release URLs in the VitaDB text are the intended URLs for tag `v1.0`; they
become usable after publication with the matching VPK filename.

No new on-device validation was performed in this preparation step. Local
regression tests do not establish full-game compatibility or visual correctness
in every field. Before publishing, check installation/update, startup, controls,
4:3/16:9 presentation, battles, movies, audio and minigames on the Vita.

The existing GitHub tag `v1.0` points to commit `a4d4cfa`, before the DEBUG
visibility correction. This rebuild includes that correction and updated release
text as local changes. The published tag was not moved. Publish the corrected
source before distributing this replacement binary as the tagged release.
