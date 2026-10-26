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

ember does not touch network code, save files or game files on disk.

## Install

ember loads with [me3](https://github.com/garyttierney/me3). Put `ember.dll` and `ember.ini` in one folder and add the DLL to your profile as an early native:

```toml
[[natives]]
path = "dll/ember.dll"
load_early = true
```

ember reads `ember.ini` from the folder that holds the DLL and writes `ember.log` there. Missing keys keep their defaults.

eventpool uses code addresses from one build of `nightreign.exe`. If a game update moves them, eventpool's hooks pass every call through unchanged, and the log shows no pooled events. The sfx and affinity fixes do not depend on the game build.

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
- `src/util.c` has the paths, settings and log.
- `src/eventpool.c` and `src/sfx.c` are the two fixes written for ember.
- `src/affinity/` is the vendored affinity fix. Its changes from upstream are listed at the top of `affinity_fix.c`.

## License

AGPL-3.0. See [LICENSE](LICENSE).

`src/affinity/` comes from [nightreign-intel-affinity-patch](https://github.com/oskarwirga/nightreign-intel-affinity-patch) by Oskar Wirga and stays under Apache-2.0. See [src/affinity/LICENSE](src/affinity/LICENSE).
