# KH2 Reaction Viewer

A Windows viewer for Kingdom Hearts II Final Mix reaction commands. Play Sora and the target together, follow multi-step reactions, and inspect command IDs, animation tracks, and BDX scripts from your own copy of the game.

## Run the viewer

1. Extract the Windows ZIP from [GitHub Releases](https://github.com/Yokimitsuro/KH2-Reaction-Viewer/releases/latest). Open `KH2 RC Viewer.exe` and keep `ReactionCatalog.tsv` beside it.
2. Choose **Open game data**. Select the KH2 installation or its `Image/dt` directory. For PC packages, the viewer reads the archive table from the game executable next to `Image`. An extracted KH2 directory containing `03system.bin` and `obj` also works.
3. Open **Reactions** to search by command name, ID, or target. **Paired only** lists commands with linked player and target tracks. **Playable only** includes the other preview types. Open **Chains** and choose **Play full chain** to watch linked steps.
4. Drag to orbit, scroll to zoom, and turn off **Follow action** for fixed framing. **Show technical details** opens the IDs, animation slots, and BDX views.

The viewer reads the game's files directly and caches previously opened data under `ViewerData/cache`. The viewer disassembles BDX scripts and reads English command names directly from the game data. Bring your own game files.

## Chain previews

- **Axel:** Burst Frontier → Overtaker → Clear Light. The viewer aligns and blends the separate clips at the transitions.
- **Firagun:** Volcanic Lord's swing and finisher run alongside Sora's tracks.
- **Blizzagun:** Blizzard Lord's swing and finisher run alongside Sora's tracks. Sora blends into his standard landing while the boss finishes.
- **Rapid Thruster:** Speed Trap → Aero Blade. The viewer uses the original idle flight with approximate placement because the enemy moves dynamically in the game.

The preview supplies its own camera and actor placement. Duo playback uses linked tracks for both actors. When the command has multiple linked phases, choose a **Paired track**. Captures authored for Roxas or Lion Sora use their own rigs. Their previews currently show the body tracks.

**Play with target for context** plays the captured player animation beside a posed target. Other entries offer the target's command activation animation. Script references show the command branch in **Show technical details**.

## Build from source

Use an x64 Native Tools Command Prompt with Visual Studio 2022 or newer, CMake 3.28 or newer, and Ninja. CMake fetches SDL3 and Dear ImGui on the first build.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable and `ReactionCatalog.tsv` are in `build`. Run `cmake --install build --prefix dist` to stage the app, catalog, and license notices.

## Credits

The viewer uses model and motion readers from [khdays-port](https://github.com/Yokimitsuro/khdays-port). See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for revisions, licenses, and other dependencies.

AI tools assisted development. The viewer runs locally without AI services or models.
