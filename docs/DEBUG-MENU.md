# Developer access to DEBUG

The DEBUG tab is hidden by default in v1.0. It is excluded from both drawing
and L/R navigation. The standard menu contains only CHEATS and GAMEPLAY.
All debug tools remain in the source code.

For developer access, create an empty file named `debug_menu.txt` in
`ux0:data/ffvii-vita/`, then restart the game. Open the menu with L + R + Start
and use L/R to reach DEBUG. Its minigame shortcuts, recorded scenes, profiler
and A/B tools remain available.

Remove the file and restart the game to hide the tab again. The marker is
not included in the VPK, and no activation option is shown in the menu.
This is a developer opt-in mechanism, not user authentication.
