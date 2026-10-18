# Name
khdays-vita

# Version
0.9.0

# Changelog (Only if update)
- Cutscenes with 3D on both screens run at full speed and full 3D resolution
- 60 fps mode runs as fast as the GPU allows instead of falling back to 30 fps
- Mission Mode enemy HP choices (Original, 2.5x, 2x, 1.5x, Story)
- Experimental single screen: widescreen scenes with the bottom screen shown over them when it plays something, and a new field HUD
- Fixed: crash on day 255
- Known issue: the new single screen mode is heavy on the GPU and often keeps 60 fps mode at 30-40 fps in busy areas

# Screenshots (Optional)
-- Insert images here --

# Author
Gustavo Garcia (ggdrn)

# AI status
AI assisted

# Description
khdays-vita is a native port of Kingdom Hearts 358/2 Days (Nintendo DS) to the PS Vita, built from the khdays-decomp decompilation project. The game code is recompiled for the Vita rather than emulated; the DS hardware it relies on (2D and 3D graphics, sound, cartridge access, touch screen) is reimplemented on top of VitaSDK and vitaGL.

Both DS screens are shown in configurable layouts, with an experimental single-screen mode that shows scenes in widescreen and brings the bottom screen over them when it is in use. The 3D is rendered at up to four times the original resolution, with widescreen 3D in the field and an experimental 60 fps mode. Story mode and Mission Mode are playable; multiplayer is not available.

# Short Description
Native PS Vita port of Kingdom Hearts 358/2 Days, built from its decompilation.

# Download Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/download/v0.9.0/khdays-vita-0.9.0.vpk

# Release Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/tag/v0.9.0

# Sourcecode Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA

# Data Files Link (Optional)
None: the game data comes from the user's own cartridge dump.

# Requirements (Optional)
- libshacccg.suprx
- Game Data Files: Kingdom Hearts 358/2 Days, European cartridge dump (YKGP), at ux0:data/khdays/days.nds
- kubridge.skprx (optional, for crash reports)
