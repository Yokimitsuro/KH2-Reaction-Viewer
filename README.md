# KH2 Reaction Viewer

A Windows viewer for Kingdom Hearts II Final Mix reaction commands. Play Sora and the target together, follow multi-step reactions, and inspect command IDs, animation tracks, and BDX scripts from your own copy of the game.

## Run the viewer

1. Download the Windows x64 ZIP and extract it. Keep the `Decoder` folder and `ReactionCatalog.tsv` beside `KH2 RC Viewer.exe`. [Latest release](https://github.com/Yokimitsuro/KH2-Reaction-Viewer/releases/latest)
2. Open the executable and choose **Open game data**. Select the Steam installation's `Image/dt` directory or an extracted KH2 directory containing `03system.bin` and `obj`.
3. Open **Reactions** to search by command name, ID, or target. Open **Chains** and choose **Play full chain** to watch linked steps.
4. Drag to orbit, scroll to zoom, and turn off **Follow action** for fixed framing. **Show technical details** opens the IDs, animation slots, and BDX views.

The viewer reads the game's files directly and caches previously opened data under `ViewerData/cache`. The Windows ZIP includes the OpenKH decoder and its runtime. Bring your own game files.

## Chain previews

- **Axel:** Burst Frontier → Overtaker → Clear Light. The viewer aligns and blends the separate clips at the transitions.
- **Firagun:** Volcanic Lord's swing and finisher run alongside Sora's tracks.
- **Blizzagun:** Blizzard Lord's swing and finisher run alongside Sora's tracks. Sora blends into his standard landing while the boss finishes.
- **Rapid Thruster:** Speed Trap → Aero Blade. The viewer uses the original idle flight with approximate placement because the enemy moves dynamically in the game.

The preview supplies its own camera and actor placement. Duo playback is available for the reactions with matched tracks. Other catalog entries can still provide IDs, target animations, or script references.

## Build from source

Use an x64 Native Tools Command Prompt with Visual Studio 2022 or newer, CMake 3.28 or newer, and Ninja. CMake fetches SDL3 and Dear ImGui on the first build.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable and `ReactionCatalog.tsv` are in `build`. Copy the `Decoder` directory from the Windows release next to the executable for the BDX and message decoders. To stage a portable build with CMake, set `KH2_RC_DECODER_DIR` to that directory when configuring, then run `cmake --install build --prefix dist`.

## Credits

The viewer uses model and motion readers from [khdays-port](https://github.com/Yokimitsuro/khdays-port) and script and message decoders from [OpenKH](https://github.com/OpenKH/OpenKh). See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for revisions, licenses, and other dependencies.

AI tools assisted development. The viewer runs locally without AI services or models.
