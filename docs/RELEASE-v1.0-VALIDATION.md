# v1.0 release validation

## Prepared artifacts

- GitHub release text: `docs/RELEASE-v1.0.md`.
- VitaDB submission text: `docs/VITADB-v1.0.md`.
- Release tag to use when publishing: `v1.0`.
- VPK: `build/VPK/ff7_vita_v1.0.vpk`.
- SHA-256 file: `build/VPK/ff7_vita_v1.0.vpk.sha256`.
- VPK size: 3,101,380 bytes.
- SHA-256: `f09c51f2b46b9fa32bf1719907070a6ac4c88d073d174ea52b97f050fba0ce97`.
- Application version: `01.00`; title ID: `FFSQ00007`.

## Completed checks

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

The build includes the current local changes in this checkout. Commit the
reviewed source and documentation before tagging the release so the published
source matches the binary.
