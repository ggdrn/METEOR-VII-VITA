# FINAL FANTASY VII for PS Vita — v1.3.5

## Crash-safe PlayStation prompt hotfix

The v1.3.4 core dump showed the game thread stuck in `fw_HeapAlloc` while
traversing the emulated heap. The final-renderer hook had not been installed
because its Thumb prologue constant was byte-swapped, but the v1.3.3 per-frame
window patch was still active. It wrote field strings from the main thread
while the game thread could free or reuse their allocations during a field
transition, which was unsafe and is consistent with the heap hang after the
Junon parade.

This release:

- removes all per-frame writes to active-window allocations;
- fixes the checked renderer prologue (`0x4FF0E92D`), enabling the intended
  final text-parser hook;
- applies prompt substitutions synchronously on the game thread;
- saves and restores the original string around the parser call, leaving the
  game's heap contents unchanged after rendering.

The log must contain `final field-text renderer hook installed`. Successful
conversions continue to use the `renderer text at ...` diagnostic.
