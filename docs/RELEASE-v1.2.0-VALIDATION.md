# v1.2.0 — validation record

## Release artifacts

- Port version: `1.2.0`; SFO application version: `01.20`.
- Source tag: `v1.2.0`.
- VPK: `build/VPK/ff7_vita_v1.2.0.vpk`.
- Compressed VPK size: 3,104,554 bytes.
- SHA-256: `9b20d06bf0b747c58f847056340cc9f666f1d5e95a8a48e4fc0ff7a1e19fde24`.
- Checksum: `build/VPK/ff7_vita_v1.2.0.vpk.sha256`.
- English release notes: `docs/RELEASE-v1.2.0.md`.
- English VitaDB submission text: `docs/VITADB-v1.2.0.md`.

## Automated checks

- `python3 scripts/test_encounters.py`: passed. Covers real C rate wrappers,
  settings round-trip and compatibility, rate restoration, saturation, adjacent
  byte isolation, independent touch-button visibility and touch OFF/ON behavior.
- `python3 scripts/emu/test_encounter_rate.py build/libjni_ff7.so`: passed.
  Original ARM field probability/world rate checks: 63 cases.
  Additional world caller simulation: 10,000 identical checks per setting;
  NORMAL = 1,005, 2X = 1,459, 4X = 2,096 encounters.
- Release build: `cmake --build build -j4`, passed with the softfp VitaSDK.
- `scripts/check-vpk.sh build/VPK/ff7_vita_v1.2.0.vpk`: passed; port files only.
- Packaged `sce_sys/param.sfo`: `APP_VER=01.20`, `TITLE_ID=FFSQ00007`.
- `git diff --check`: passed.

## Device evidence and remaining validation

A user-provided device log confirms installation of both multiplier hooks.
The user tested 4X on the world map but did not perceive a change. That build
logged neither the active factor during checks nor effective rates, so the log
cannot confirm the effect while walking. This release includes those diagnostics.

The simulation demonstrates that calculation multipliers do not translate to
linear increases in battle counts. The release notes disclose this behavior.
On-device frequency comparison, materia combinations, save/reload behavior,
area transitions and scripted-battle regression checks remain pending.

## Distribution

The source tag and documentation do not upload the VPK to GitHub Releases or
submit anything to VitaDB. The download URL in the VitaDB text is the intended
asset URL and requires uploading the VPK to the matching GitHub Release.
