# FINAL FANTASY VII for PS Vita — v1.3.6

Fix PlayStation prompts with stock Android game data. Field dialog tables
can follow the stored script length; use the next section offset as the
validated boundary instead of rejecting `strings offset == script length`.
Recognize stock `[A]`, `[B]`, `[X]`, `[Y]` labels with Android's physical
mapping (Cross, Circle, Square, Triangle), preserving the translation's
existing parenthesized mapping.

Regression coverage includes split x86 pages, stock and mod section layouts,
invalid boundaries, all four square-bracket labels and idempotence. Device
visual validation remains necessary.

Validation: host regression tests passed against 702 stock fields (1,275
replacements) and 702 modded fields (194 replacements), with no writes
outside dialog tables and no changes on a second patch pass. The Release
VPK built successfully and passed package and ZIP CRC checks.
