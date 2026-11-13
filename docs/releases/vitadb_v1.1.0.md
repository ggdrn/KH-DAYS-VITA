# Name
khdays-vita

# Version
1.1

# Changelog (Only if update)
- Enabled Roxas's Oathkeeper and Oblivion dual-wield form in Story Mode mission replays and Challenges when Zero Gear and three Ability Units are equipped.
- Fixed `LOCKED ON` and portal countdown text being clipped in DETAIL single-screen mode.
- Fixed shortcut-command graphics corruption when holding L with the widescreen 4:3 HUD below 100% size.
- Fixed black flashes when scripted cutscenes changed cameras.
- Fixed the oversized duplicate Genie during Mission 24 dialogue camera changes.
- Fixed moving seams in repeating skybox textures, including the Twilight Town sky.
- Improved scripted-scene stability by presenting authored 30 Hz camera frames directly while retaining gameplay interpolation.

# Validation note
- Version 1.1 was built as a native Vita package and passed video-composition tests, ending-link checks, linked-ELF post-credits scene validation and VPK integrity validation.
- The fixes were exercised on real Vita hardware during development; additional mission-by-mission testing remains welcome.
- The game requires the user's own European Kingdom Hearts 358/2 Days cartridge dump (`YKGP`). No game data is included.

# Author
Gustavo Garcia (ggdrn)

# AI status
AI assisted

# Description
khdays-vita is a native port of Kingdom Hearts 358/2 Days (Nintendo DS) to the PS Vita, built from the khdays-decomp decompilation project. The game code is recompiled for Vita; the DS 2D/3D graphics, sound, cartridge access and touch input are reimplemented for the Vita runtime.

# Short Description
Native PS Vita port of Kingdom Hearts 358/2 Days, built from its decompilation.

# Download Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA/releases/download/1.1/khdays-vita-1.1.0.vpk

# Release Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/tag/1.1

# Sourcecode Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA

# Data Files Link (Optional)
None: the game data comes from the user's own cartridge dump.

# Requirements (Optional)
- libshacccg.suprx
- Game Data Files: Kingdom Hearts 358/2 Days, European cartridge dump (`YKGP`), at `ux0:data/khdays/days.nds`
- kubridge.skprx (optional, for crash reports)
