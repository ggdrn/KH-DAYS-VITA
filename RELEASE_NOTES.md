# khdays-vita v0.1.0

First release of the native PS Vita port of *Kingdom Hearts 358/2 Days*. The DS game is
recompiled for the Vita, not emulated. Bring your own European dump (`YKGP`); see the README.

## Playable

- Story mode, with saves compatible with DS emulator raw saves.
- Mission Mode (solo).
- Dual-screen 3D cutscenes, videos and the game's own screen transitions.

## Graphics

- 3D rendered up to 4x the DS resolution (3x by default).
- 60 fps: frames interpolated between the game's 30.
- Widescreen 16:9 3D in the field, with the HUD stretched or kept 4:3.
- Smoothed 3D textures, with clean cut-out edges.
- 2D filter for sprites, text and menus: Pixel, Sharp or Smooth (default).
- Screen effects: scanlines or LCD grid.
- Screen layouts: top screen large, bottom screen large, or side by side; size, corner and
  opacity of the small screen; tap it to swap.
- Optional FPS counter.

## Controls

- Every DS button remappable to any Vita button.
- Right stick turns the field camera (speed, invert X/Y).
- D-pad can move the command deck cursor while the left stick moves.
- R can toggle lock-on instead of being held.
- Rear touchpad halves as L/R or Select/Start.
- Adjustable stick dead zone.
- Fast-forward with L+R+Square (2x or 3x).

## Port menu (L+R+Select)

In-game settings in five tabs, switched with L/R: Video, Screen, Controls, Buttons, System.
Quality presets (Performance, Balanced, Quality), volume, restore defaults. Everything is
saved to `ux0:data/khdays/config.ini`.

## System

- Fast boot: compiled shaders cached, the ROM check skipped once verified.
- LiveArea bubble.

## Known limits

- Multiplayer is not available.
- Some heavy scenes (dual-screen 3D cutscenes) can drop below 60 fps.
