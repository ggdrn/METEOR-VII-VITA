# v1.1.0 — validation record

## Prepared artifacts

- Release notes: `docs/RELEASE-v1.1.0.md`.
- VitaDB submission text: `docs/VITADB-v1.1.0.md`.
- VPK size: 3,102,954 bytes.
- SHA-256: `adc0fb5afcf59dd041af0690c2f09737b123ebd85a74df8e9b1505961ec89d1c`.
- Checksum file: `build/VPK/ff7_vita_v1.1.0.vpk.sha256`.
- Source tag: `v1.1.0`.
- The VitaDB download URL is intended for a GitHub Release with the matching
  VPK asset. Pushing the source tag alone does not upload the binary or submit
  the update to VitaDB.

## Automated checks

- `python3 scripts/test_encounters.py`: native state, independent visibility,
  repeated hide/show without duplicated objects, multiple layouts, held-touch
  cleanup, hidden touch exclusion, visible touch persistence, existing settings
  compatibility and settings round-trip.
- Release build and `scripts/check-vpk.sh`: package contains only port files.

- `git diff --check`: passed.
- Build artifact: `build/VPK/ff7_vita_v1.1.0.vpk`.
- SFO application version: `01.10`; port version: `1.1.0`.

## Pending on-device checks

On-device validation has not been performed for this release.

1. Open Gameplay with L + R + Start and select RANDOM ENCOUNTERS.
2. Turn encounters OFF and ON in a field and on the world map. Verify the
   on-screen button reflects the menu setting.
3. Touch the visible button and verify the menu reflects the new encounter state.
4. Hide the button with encounters ON and OFF. Verify the setting stays unchanged
   and touching its former position does not toggle encounters.
5. Show the button again and check for duplicate or stale touch targets, including
   after a held touch. Repeat through field, battle and minigame transitions.
6. Verify submarine L2/R2 touch controls still appear and work independently.
7. Restart with each ON/OFF and SHOW/HIDE combination and verify persistence.
8. Update an existing installation without the new configuration keys and verify
   the defaults are encounters ON and button SHOW.
