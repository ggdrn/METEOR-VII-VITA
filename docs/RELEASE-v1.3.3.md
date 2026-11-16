# FINAL FANTASY VII for PS Vita — v1.3.3

## PlayStation prompts in every field-text language

The v1.3.2 device trace showed that stock Android field files can have no
embedded dialog table: the script string offset equals the script-section
length. This made the earlier LZSS-time conversion reject every tested field,
while a modified PT-BR `flevel.lgp` could behave differently.

The port now patches the text pointers installed in the four active field
windows immediately before drawing a frame. This happens after the game or a
mod has selected/provided the localized message, so it supports every language
without requiring edited game data. Colored/uncolored A/B/X/Y labels and the
original F6-F9 button opcodes remain supported.

Successful runtime replacements are logged as:

```
ff7_buttons: window N text at XXXXXXXX: N button labels -> PlayStation symbols
```
