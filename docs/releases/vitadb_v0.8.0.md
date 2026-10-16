# Name
khdays-vita

# Version
0.8.0

# Changelog (Only if update)
- New "left" screen layout and widescreen HUD at the screen edges
- 60 fps mode falls back to a steady 30 fps when it can't keep up; lighter dual-3D scenes
- Experimental: single screen for dialogue scenes and mission selection
- Experimental: partner swap (play as the story partner)
- Fixed: mission 54 crash, Possessor white flash, camp menu audio noise

# Screenshots (Optional)
-- Insert images here --

# Author
Gustavo Garcia (ggdrn)

# AI status
AI assisted

# Description
khdays-vita is a native port of Kingdom Hearts 358/2 Days (Nintendo DS) to the PS Vita, built from the khdays-decomp decompilation project. The game code is recompiled for the Vita rather than emulated; the DS hardware it relies on (2D and 3D graphics, sound, cartridge access, touch screen) is reimplemented on top of VitaSDK and vitaGL.

Both DS screens are shown in configurable layouts, with an experimental single-screen mode. The 3D is rendered at up to four times the original resolution, with widescreen 3D in the field and an experimental 60 fps mode. Story mode and Mission Mode are playable; multiplayer is not available.

# Short Description
Native PS Vita port of Kingdom Hearts 358/2 Days, built from its decompilation.

# Download Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/download/v0.8.0/khdays-vita-0.8.0.vpk

# Release Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/tag/v0.8.0

# Sourcecode Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA

# Data Files Link (Optional)
None: the game data comes from the user's own cartridge dump.

# Requirements (Optional)
- libshacccg.suprx
- Game Data Files: Kingdom Hearts 358/2 Days, European cartridge dump (YKGP), at ux0:data/khdays/days.nds
- kubridge.skprx (optional, for crash reports)
