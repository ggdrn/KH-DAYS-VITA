# Name
khdays-vita

# Version
1.0

# Changelog (Only if update)
- Fixed the post-credits save-scene descriptor, restoring the cleared-game save flow and the Theater Mode / Zero Gear unlock path.
- Added linked-ELF validation and diagnostic logging for the ending save scene.
- Fixed the mostly black display that could occur after pausing and resuming the final Riku battle.
- Fixed the brief widescreen 4:3 HUD corruption when opening the L shortcut.
- Fixed the temporary stretched Darkside tentacle geometry in Holo-Mission: Two Keys.
- Fixed pause dimming so the dark overlay also covers the widescreen side borders.
- Applied the pause-border correction to cutscene pause menus with Continue / Skip, not only battle pause menus.
- Added three one-screen choices: OFF, SIMPLE and DETAIL.
- SIMPLE uses static panels with fewer 2D texture effects and filters for better performance.
- DETAIL keeps the richer HUD composition, detailed 2D filtering and effects for a more immersive experience, at a higher rendering cost.
- Added one-screen mode-change logging and an optimization that skips smoothing pixels discarded by DETAIL masks.
- Kept the Vita at the configured performance clocks (CPU 444 MHz, GPU 222 MHz, bus 222 MHz, XBAR 166 MHz); no unsafe forced-100% utilization mode was added.

# Validation note
- Version 1.0 was built as a native Vita package and passed the post-link ending-scene check, composition tests, ending-link tests and GX3D host test.
- Performance and the cutscene Continue / Skip overlay still require confirmation on real Vita hardware.
- The game requires the user's own European Kingdom Hearts 358/2 Days cartridge dump (YKGP).

# Author
Gustavo Garcia (ggdrn)

# AI status
AI assisted

# Description
khdays-vita is a native port of Kingdom Hearts 358/2 Days (Nintendo DS) to the PS Vita, built from the khdays-decomp decompilation project. The game code is recompiled for Vita; the DS 2D/3D graphics, sound, cartridge access and touch input are reimplemented on top of VitaSDK and vitaGL.

# Short Description
Native PS Vita port of Kingdom Hearts 358/2 Days, built from its decompilation.

# Download Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA/releases/download/1.0/khdays-vita-1.0.0.vpk

# Release Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/tag/1.0

# Sourcecode Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA

# Data Files Link (Optional)
None: the game data comes from the user's own cartridge dump.

# Requirements (Optional)
- libshacccg.suprx
- Game Data Files: Kingdom Hearts 358/2 Days, European cartridge dump (YKGP), at ux0:data/khdays/days.nds
- kubridge.skprx (optional, for crash reports)
