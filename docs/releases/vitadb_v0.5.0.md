# Name
khdays-vita

# Version
0.5.0

# Changelog (Only if update)
- PlayStation button icons in text, HUD and menus (option)
- Cross confirms, Circle cancels by default (option)
- Right stick switches lock-on targets
- Mission balance option: story enemy HP and damage in solo Mission Mode
- 30 fps mode locked to a steady 30 fps
- 60 fps mode (experimental): no more scenery shaking
- Fixed cutscene freezes and the white flash on some enemy defeats
- Sound uses half the CPU; lighter movie playback

# Screenshots (Optional)
-- Insert images here --

# Author
Gustavo Garcia (ggdrn)

# AI status
AI assisted

# Description
khdays-vita is a native port of Kingdom Hearts 358/2 Days (Nintendo DS) to the PS Vita, built from the khdays-decomp decompilation project. The game code is recompiled for the Vita rather than emulated; the DS hardware it relies on (2D and 3D graphics engines, sound, cartridge access, touch screen) is reimplemented on top of VitaSDK and vitaGL.

Both DS screens are shown at once in configurable layouts, with an optional single-screen mode for missions. The 3D is rendered at up to four times the original resolution, with optional texture filtering, widescreen 3D in the field and an experimental 60 fps mode. Buttons are remappable, the right stick controls the camera and lock-on targets, and the game's button icons can be shown as PlayStation symbols. Story mode and Mission Mode are playable; multiplayer is not available. Save files are compatible with common DS emulators.

# Short Description
Native PS Vita port of Kingdom Hearts 358/2 Days, built from its decompilation.

# Download Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/download/v0.5.0/khdays-vita-0.5.0.vpk

# Release Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/tag/v0.5.0

# Sourcecode Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA

# Data Files Link (Optional)
None: the game data comes from the user's own cartridge dump.

# Requirements (Optional)
- libshacccg.suprx
- Game Data Files: Kingdom Hearts 358/2 Days, European cartridge dump (YKGP), placed at ux0:data/khdays/days.nds
- kubridge.skprx (optional, for crash reports)
