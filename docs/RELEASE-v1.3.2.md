# FINAL FANTASY VII for PS Vita — v1.3.2 diagnostic build

This build traces every LZSS output considered for the in-game PlayStation
button prompts. The log now records the output size and address, font state,
field header, and the exact validation step that rejected a block.

It also makes the cross-thread `font_ready` flag volatile and pairs it with a
memory barrier, so the game thread observes the font prepared by the
texture-loading thread.

To reproduce, enter the scene that still displays A/B/X/Y, then send the new
`logs.txt`. Searchable diagnostic lines start with `ff7_buttons: LZSS`.
