# FINAL FANTASY VII for PS Vita — v1.3.4

## Final field-text renderer conversion

The v1.3.3 log proved that eight button labels were found and converted in
the regular active-window strings, but the affected tutorial still displayed
Xbox letters. The tutorial/minigame renderer was therefore consuming a later
copy rather than those source pointers.

This release also converts labels at the entry of the field-text parser
(`func_0x006e706d`). Its fourth x86 argument is the exact text pointer the
game is about to parse and draw, covering regular dialogs and tutorial text
after language selection and intermediate copies.

Successful final-path conversions are logged as:

```
ff7_buttons: renderer text at XXXXXXXX: N button labels -> PlayStation symbols
```
