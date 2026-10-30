# ember

ember is a native mod for Elden Ring Nightreign that raises the frame rate on CPU-bound systems, mostly under Proton on Linux. Nightreign's frame rate there is limited by its main thread, and ember cuts work and waiting from that thread.

Measured on a Ryzen 5 3600 under Proton, in an offline scene with 5 runs per variant:

| Fix | Average fps | Main thread CPU per frame |
| --- | --- | --- |
| eventpool | +5.9% (62.99 to 66.72) | 0.58 ms less |
| sfx | +3.0% (66.85 to 68.83) | 0.54 ms less |

The affinity fix was not measured on that CPU. It targets stutter from engine workers pinned to a few CPUs.

## What it does

- **eventpool.** The engine wraps every kernel event in a DLEvent object, and the SFX slot code creates and destroys about 3,000 of them per second. Under Wine each `CreateEventW` and `CloseHandle` is a wineserver round trip. ember hooks both calls in the game's import table, and only for the DLEvent constructor and destructor. A destroyed event is reset and kept in a pool, and the next DLEvent gets it back. The game sees the same event semantics.
- **sfx.** LoadBalancerParam decides at which load level the game halves SFX particles and turns off SFX reflection, water interaction, distortion and render distance scaling. ember sets those levels to 0 in every row, so the reductions are always on. It finds the param in the loaded regulation by its type name, so it needs no game addresses. The change is visible: fewer particles and no SFX reflections.
- **affinity.** Nightreign pins some engine worker threads to CPUs 0 to 3 when it names them. This is the [Nightreign Intel Hybrid CPU Stutter Fix](https://github.com/oskarwirga/nightreign-intel-affinity-patch) by Oskar Wirga, with a configurable thread list. When the game names a listed thread, ember widens its CPU mask to every CPU the process may use.
- **fps.** The game writes a frame time of 1/60 s into its frame pacer. ember changes that constant to 1/`limit`, so the cap moves from 60 to `limit` fps.
- **ultrawide.** The game letterboxes resolutions wider than 16:9. ember turns the conditional jump after that aspect check into a plain jump, so wider resolutions fill the screen.
- **loadbars.** Loading screens stay 16:9, so with ultrawide on their sides show the last world frame. ember hooks the swapchain's `Present` and clears both side strips to black in the back buffer while the game updates a `CS::LoadingScreen`. It finds that class's vtable through its RTTI name, so it needs no game addresses.
- **animation.** The game updates characters outside a view frustum at a lower animation rate. ember makes the frustum builder read the far clip distance where it reads the aspect ratio, so the frustum is almost 180 degrees wide and characters at the screen edges and beside the camera animate at full rate. It costs about 2% fps (67.44 to 66.21 in one run each). This is the Nightreign version of [IncreaseAnimationDistance](https://github.com/techiew/EldenRingMods/tree/master/IncreaseAnimationDistance) by techiew.
- **postfx.** Removes chromatic aberration, the vignette and the sharpen filter. The game registers chromatic aberration and the vignette under UTF-16 labels such as `GS Presents - Vignetting`, and ember moves each `lea r9` that loads one of those labels 2 characters into the string. The game then finds no setting under that name and leaves the effect off. The original mods replace the `lea` with NOPs, which leaves r9 holding whatever came before and crashes on Windows when that is 0. For the sharpen filter it blanks the file name `SharpenFilter.ppo`, so the game never loads that pass. These are the same edits as huutaiii's Remove Chromatic Aberration, Remove Vignette and Disable Sharpen Filter mods, but ember finds the instructions through the label strings instead of fixed offsets.

fps, ultrawide and animation find their instruction by byte pattern when ember loads and log the address. The fps and ultrawide patterns come from [ReignTweak](https://github.com/Minksh/ReignTweak) by Minksh; the ultrawide pattern is originally from [techiew's EldenRingMods](https://github.com/techiew/EldenRingMods).

ember does not touch network code, save files or game files on disk.

## Install

ember loads with [me3](https://github.com/garyttierney/me3). Put `ember.dll` and `ember.ini` in one folder and add the DLL to your profile as an early native:

```toml
[[natives]]
path = "dll/ember.dll"
load_early = true
```

ember reads `ember.ini` from the folder that holds the DLL and writes `ember.log` there. Missing keys keep their defaults.

eventpool uses code addresses from one build of `nightreign.exe`. If a game update moves them, eventpool's hooks pass every call through unchanged, and the log shows no pooled events. The sfx and affinity fixes do not depend on the game build. If an update changes the fps, ultrawide, animation or postfx code, ember logs an error and leaves it alone.

## Settings

| Section | Key | Default | Effect |
| --- | --- | --- | --- |
| eventpool | enabled | 1 | Pool the DLEvent kernel events. |
| sfx | enabled | 1 | Set the SFX reduction levels in LoadBalancerParam. |
| sfx | particle_half | 0 | Load level from which SFX particles are halved. 0 means always, 20 means never. |
| sfx | reflection | 0 | Load level from which SFX reflection is off. |
| sfx | water_interaction | 0 | Load level from which SFX water interaction is off. |
| sfx | distortion | 0 | Load level from which SFX distortion is off. |
| sfx | scale_render_distance | 0 | Load level from which SFX render distance is scaled down. |
| sfx | scan_seconds | 120 | How long to look for the loaded param after launch. |
| fps | limit | 144 | Frame rate cap. 0 keeps the game's 60. |
| ultrawide | enabled | 1 | Fill screens wider than 16:9. |
| loadbars | enabled | 1 | Black side bars on loading screens on displays wider than 16:9. |
| animation | enabled | 1 | Full-rate animation outside the view. |
| postfx | remove_chromatic_aberration | 1 | Remove chromatic aberration. |
| postfx | remove_vignette | 1 | Remove the darkened screen edges. |
| postfx | remove_sharpen | 1 | Remove the sharpen filter. |
| affinity | enabled | 1 | Widen the CPU masks of the listed engine workers. |
| affinity | threads | 11 worker names | Comma-separated thread names to widen. See `ember.ini`. |
| affinity | log | 0 | Write a trace of every affinity call to `ember-affinity-<pid>.jsonl`. |
| log | level | error | `off`, `error`, `warn`, `info` or `debug`. Debug adds eventpool counts every minute. |

## Build

With Nix:

```sh
nix run .      # builds ember.dll in this folder
nix build      # builds ember.dll and ember.ini into ./result
nix develop    # shell with the MinGW toolchain, then run make
```

Without Nix, any MinGW-w64 GCC works:

```sh
make CC=x86_64-w64-mingw32-gcc
```

The DLL links statically and imports only Windows system DLLs.

## Layout

- `src/main.c` checks that it runs in `nightreign.exe` and starts each fix.
- `src/util.c` has the paths, settings, log, the code pattern search and the import hook.
- `src/eventpool.c` and `src/sfx.c` are the two fixes written for ember.
- `src/fps.c`, `src/ultrawide.c` and `src/animation.c` are one-instruction patches found with `aob_find` in `util.c`.
- `src/postfx.c` finds the post-processing effects by their names in the exe's strings and turns them off.
- `src/loadbars.c` hooks DXGI and draws the loading screen bars with D3D12.
- `src/affinity/` is the vendored affinity fix. Its changes from upstream are listed at the top of `affinity_fix.c`.

## License

AGPL-3.0. See [LICENSE](LICENSE).

`src/affinity/` comes from [nightreign-intel-affinity-patch](https://github.com/oskarwirga/nightreign-intel-affinity-patch) by Oskar Wirga and stays under Apache-2.0. See [src/affinity/LICENSE](src/affinity/LICENSE).
