# Soldiers: Heroes of World War II: Enhancement Pack

A fan-made patch for the 2004 game *Soldiers: Heroes of World War II*.

It upgrades the last known GOG release to include the Enhanced Edition's features, HD textures,
a 4K interface and modern WASD controls, and it removes the Steam dependencies that the Enhanced
Edition introduced.

Note: the GOG version this patch targets (1.28.3E, build 18993) was delisted on 29 June 2026, and the
Steam version (Enhanced Edition) was delisted at the end of June 2026. Since then, the game can
no longer be bought from any digital store.

## Features

- **Enhanced Edition content (R1.28.8):** improved aiming, four new bonus missions, unit fixes and
  smarter enemy targeting.
- **4K interface:** readable menus, crisp text and redrawn artwork.
- **Modern frame-rate options:** V-Sync, 240, 120, 60 or 30 FPS.
- **HD textures** (the HD Project workshop mod by Suffskin), built in.
- **Modern WASD controls** for the camera and direct control.
- **Extended camera zoom** (thanks to zocker_160's widescreen fix).
- **Up to 64 sounds at once** instead of 20, so big battles no longer drop gunfire and explosions.
- **Linux/Proton fixes** for music and video playback.
- **No logo videos on startup.**
- **DRM-free:** runs on its own, no Steam or launcher required.

## Requirements

- The GOG version of the game (1.28.3E).
- A 4K monitor to appreciate the upscaled 4K interface; otherwise the game falls back to its original
  interface size.

## Installation

1. Download all three parts from the [Releases page](../../releases).
2. Unpack each part into your game folder. Choose "yes" when asked to overwrite files.
3. Linux only: set this environment variable in your game launcher: `WINEDLLOVERRIDES=d3d9=n,b`
4. Start the game.

## Credits

- **zocker_160**: [widescreen fix](https://github.com/zocker-160/Soldiers-HoWW2-widescreen-fix) (camera zoom).
- **Suffskin**: [HD Project](https://steamcommunity.com/sharedfiles/filedetails/?id=3412071642) textures.
- **Best Way**: developer of the game and the Enhanced Edition.
- **Fulqrum Publishing**: publisher of the Enhanced Edition.
- **1C** and **Codemasters**: publishers of the original 2004 release.
- **Zenju**: xBRZ image scaler.
- **DejaVu, Liberation, URW**: fonts.
- **GOG**: DRM-free release of the game.

## Licence

- **The pack's code:** GPL-3.0 (see [LICENSE](LICENSE)), because it builds on GPL-3.0 code by zocker_160
  and Zenju.
- **The game's program, data, music, videos and artwork:** not covered by this licence. They remain the
  property of Best Way and the game's publishers, and are included only because the game is no longer sold.
- **The HD Project textures:** not covered by this licence. They remain the property of Suffskin and are
  included unchanged.
