# KH2 Reaction Viewer

A Windows viewer for Kingdom Hearts II Final Mix reaction commands. Play Sora and the target together, follow multi-step reactions, and inspect command IDs, animation tracks, and BDX scripts from your own copy of the game.

## Run the viewer

1. Extract the Windows ZIP from [GitHub Releases](https://github.com/Yokimitsuro/KH2-Reaction-Viewer/releases/latest). Open `KH2 RC Viewer.exe` and keep `ReactionCatalog.tsv` beside it.
2. Choose **Open game data**. Select the KH2 installation or its `Image/dt` directory. For PC packages, the viewer reads the archive table from the game executable next to `Image`. An extracted KH2 directory containing `03system.bin` and `obj` also works.
3. Open **Reactions** to search by command name, ID, or target. The list opens on **Paired only**, which shows previews that animate the player and target together. **Playable only** includes other preview types. Open **Chains** and choose **Play full chain** to watch linked steps.
4. Drag to orbit, scroll to zoom, and turn off **Follow action** for fixed framing. **Show technical details** opens the IDs, animation slots, and BDX views.

The viewer reads the game's files directly and caches previously opened data under `ViewerData/cache`. The viewer disassembles BDX scripts and reads English command names directly from the game data. Bring your own game files.

PC previews use the installed remaster textures where available. Wild Dance uses the target's own animation in the standard and Data variants. Hydra's Phil One-Two and Urninator also play the urn's track.

**PAX effects** plays supported particle and weapon effects from the original animation cues. The viewer reads the object effect banks from the game and replays them when seeking. Some effects require battle state or screen distortion and are unavailable in the preview.

## Chain previews

- **Axel:** Burst Frontier → Overtaker → Clear Light. The viewer aligns and blends the separate clips at the transitions.
- **Barbossa:** Twin Counter → Sonic Dive. Sora, Jack Sparrow, and Barbossa each play their own motion.
- **Demyx:** Wild Dance's catch and follow-up.
- **Cyclone:** Sora's dodge and dive play in sequence. The Soldier uses its original kick and a hit animation with preview timing and placement.
- **Hydra:** Phil One-Two → Urninator. **Vanquish** is available separately under **Reactions**.
- **Xemnas:** All three authored phases of the final laser barrage.
- **Firagun:** Volcanic Lord's swing and finisher run alongside Sora's tracks.
- **Blizzagun:** Blizzard Lord's swing and finisher run alongside Sora's tracks. Sora blends into his standard landing while the boss finishes.
- **Rapid Thruster:** Speed Trap → Aero Blade. The viewer uses the original idle flight with approximate placement because the enemy moves dynamically in the game.

The preview supplies its own camera and reconstructs actor placement from the available tracks. When the command has multiple linked phases, choose a **Paired track**. Final Xemnas reactions that include Riku use his track from the same capture package. Captures authored for Roxas or Lion Sora use their own rigs and currently show their body tracks.

**Play player animation** plays a capture without a linked target action. **Show static target for reference** is available in technical details for these captures when the target model is present. Other entries offer the target's command activation animation. Script references show the command branch in **Show technical details**.

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
