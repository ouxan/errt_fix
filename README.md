# errt_fix

Fixes ray tracing stutter in Elden Ring (PC, DirectX 12).

With ray tracing enabled, Elden Ring relinks its entire ray tracing pipeline on the render thread
whenever a new material appears on screen. Each relink takes tens of milliseconds, which causes
repeated frame-time spikes in dense areas such as forests. errt_fix prepares a pipeline that already
contains every material, so these relinks no longer happen during gameplay.

> **Offline use only.** errt_fix modifies the game process. Use it only with Easy Anti-Cheat
> disabled, in offline mode. Do not go online with it installed. See the [legal notice](#legal-notice).

## Features

| Feature | Setting | Description |
|---------|---------|-------------|
| RT pipeline cache | `rt_cache` | Records the game's RT material collections to disk and links a pipeline containing all of them in the background at startup. Requests from the game are answered with this prebuilt pipeline. |
| Material scan | `rt_scan` | Builds the complete cache in one session by finding every RT material shader in process memory. |
| BLAS refit thinning | `refit_thinning` | Animated foliage acceleration structures are updated every second frame instead of every frame. |
| Pipeline state reuse | `pso_reuse` | A pipeline the game requests more than once is created from the driver binary of the first compile. |

Measured on an RX 9070 XT in a forest area: the worst frame dropped from 1035 ms to 61 ms, and BLAS
refit thinning saves about 1 ms per frame.

## Installation

1. Copy `dinput8.dll` and `errt_fix.ini` next to `eldenring.exe` (`ELDEN RING\Game\`).
2. Create `steam_appid.txt` in the same folder containing `1245620`, and start `eldenring.exe`
   directly (not `start_protected_game.exe`). The game starts in offline mode without Easy Anti-Cheat.
3. Build the cache once: set `rt_scan=1` in `errt_fix.ini`, start the game, wait about one minute in
   the main menu, quit, and set `rt_scan=0` again.

The cache is stored in `Game\errt_fix\rt_pipeline_cache.bin`. It contains DXIL and pipeline
descriptions only, so it works on both AMD and NVIDIA hardware. The first start after building it
links the full pipeline in the background (about 30 seconds); later starts take under a second
thanks to the driver's shader cache.

Materials that are not in the cache are added automatically when the game first uses them.

## Log

`Game\errt_fix\events.log` lists cache activity, pipeline compiles longer than 20 ms, and a summary
every five minutes. A line beginning with `RT CACHE: new material recorded` means a material was
missing from the cache; it is included from the next start on.

## Building

Requirements: Visual Studio 2022 (MSVC, Windows SDK) and CMake 3.20 or newer.

```bat
build.bat
```

The DLL and the default `errt_fix.ini` are written to `dist\`.

| File | Purpose |
|------|---------|
| `src/main.cpp` | `dinput8.dll` proxy, configuration, logging |
| `src/hooks.cpp` | D3D12/DXGI vtable hooks, BLAS refit thinning, overlay-safe Present hook |
| `src/rt_cache.cpp` | RT pipeline cache and material scan |
| `src/pso_reuse.cpp` | Pipeline state reuse |
| `src/vtidx.c` | Vtable slot indices taken from the Windows SDK headers |

## License

errt_fix is released under the [MIT License](LICENSE). You may use, modify, fork and redistribute
it, including in other projects, provided that the copyright notice and the license text are
retained in all copies or substantial portions of the software. When publishing a fork or a
derived mod, please also credit this project with a link to the original repository.

## Legal notice

- **No affiliation.** errt_fix is an independent, unofficial project. It is not affiliated with,
  endorsed, sponsored or approved by FromSoftware, Inc., Bandai Namco Entertainment Inc., or any of
  their affiliates. ELDEN RING and all related names and marks are trademarks of their respective
  owners and are used here only to identify the game this software is compatible with.
- **No game content.** This repository contains only original source code. It does not include,
  and must not be used to distribute, any code, shaders, assets or other files belonging to the
  game. The cache file (`rt_pipeline_cache.bin`) is generated locally from the user's own installed
  copy of the game and must not be redistributed.
- **Offline use only.** errt_fix loads into the game process and changes how it uses the graphics
  API. It is intended solely for offline single-player use with Easy Anti-Cheat disabled. Using
  modified game files or processes in online play may violate the game's terms of service and can
  result in account restrictions or bans. Do not use errt_fix online.
- **User responsibility.** You are solely responsible for ensuring that your use of this software
  complies with the End User License Agreement and terms of service of the game and of any platform
  you use, and with the laws that apply to you.
- **No warranty.** The software is provided "as is", without warranty of any kind, as stated in
  the [license](LICENSE). It may cause crashes, graphical errors or other malfunctions. Back up
  your save files before use. In no event shall the authors or contributors be liable for any
  claim, damages, account action, data loss or other liability arising from the use of this
  software.
