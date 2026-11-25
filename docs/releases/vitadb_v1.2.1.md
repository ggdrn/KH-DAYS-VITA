# Name
khdays-vita

# Version
1.2.1

# Changelog (Only if update)
- Fixed Mission 79 never loading the next room after meeting Xigbar in Halloween Town (a crash while creating the Detonator enemy).
- Modified copies of the European cartridge are now accepted, such as an undub with the Japanese voices and cutscenes or a fan translation, as long as the gamecode stays `YKGP`. The first boot with one shows a notice that the ROM is modified.
- Dumps rebuilt by tools such as ndstool, with a size other than 256 MiB, are supported.

# Validation note
- Version 1.2.1 was built as a native Vita package. The Detonator fix was checked against the original cartridge code, and Mission 79 was played on real Vita hardware through completion and save with no faults.
- Undub support has not yet been tested with an actual undub dump on hardware; reports with `ux0:data/khdays/log.txt` are welcome.
- The game requires the user's own European Kingdom Hearts 358/2 Days cartridge dump (`YKGP`), unmodified or modified. No game data is included.

# Author
Gustavo Garcia (ggdrn)

# AI status
AI assisted

# Description
khdays-vita is a native port of Kingdom Hearts 358/2 Days (Nintendo DS) to the PS Vita, built from the khdays-decomp decompilation project. The game code is recompiled for Vita; the DS 2D/3D graphics, sound, cartridge access and touch input are reimplemented for the Vita runtime.

# Short Description
Native PS Vita port of Kingdom Hearts 358/2 Days, built from its decompilation.

# Download Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA/releases/download/1.2.1/khdays-vita-1.2.1.vpk

# Release Link
https://github.com/ggdrn/KH-DAYS-VITA/releases/tag/1.2.1

# Sourcecode Link (Optional)
https://github.com/ggdrn/KH-DAYS-VITA

# Data Files Link (Optional)
None: the game data comes from the user's own cartridge dump.

# Requirements (Optional)
- libshacccg.suprx
- Game Data Files: Kingdom Hearts 358/2 Days, European cartridge dump (`YKGP`, unmodified or modified, e.g. an undub), at `ux0:data/khdays/days.nds`
- kubridge.skprx (optional, for crash reports)
