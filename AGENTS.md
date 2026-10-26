# Agent notes

ember is a Windows DLL for Elden Ring Nightreign, written in C and cross-compiled with MinGW. Read README.md first for what each fix does.

## Build

- `nix run .` builds `ember.dll` in the repo root. `nix build` builds it with `ember.ini` into `./result`.
- Inside `nix develop`, run `make`. The bare MinGW gcc wrapper needs `-L<mcfgthreads>/lib`, which the flake app adds for you.
- `make` must build without warnings (`-Wall`).
- `ember.dll` and `result` are ignored. Never commit them.

## Code

- Each fix is one file in `src/` with a `<fix>_start()` function, declared in `src/ember.h` and called from `DllMain` in `src/main.c` when its `[<fix>] enabled` key is 1.
- Settings are read with `cfg(section, key, default)` from `ember.ini` next to the DLL. A new key also needs a line in `ember.ini` and in the README settings table.
- Log with `log_line(level, ...)` into `ember.log`. Keep per-call or per-frame output at `LOG_DEBUG`.
- Game code addresses are absolute (the exe loads at 0x140000000). Name what an address is in a short comment. Prefer finding data at runtime, as `sfx.c` does, over hard-coded addresses.
- `src/affinity/` is vendored Apache-2.0 code. Keep its upstream style, and list every change in the comment at the top of `affinity_fix.c`.
- Comments are at most two lines and only explain what is not obvious from the code.
- Match the existing style: dense C, early returns, fixed buffers where the size is known.

## Safety

- Hooks run on the game's hottest paths. A hook must pass through every call it does not own unchanged, and must never block.
- ember must not change network code, save files or files on disk outside its own log.
- Do not test a modified build online. Test with a profile that does not connect to FromSoftware's servers.

## Testing

There are no unit tests. Test in game:

1. Load the DLL early from an me3 profile and set `[log] level=info`.
2. Check `ember.log`: `eventpool import hooks 2 of 2`, `fps limit`, `ultrawide aspect check skipped`, and `sfx patched` with a row count. At `debug`, eventpool logs reused and created events every minute; after loading, `created` should stay near 0.
3. For performance changes, compare average fps, 1% low frame time and main thread CPU per frame on the same scene and save, with at least 5 runs per variant, interleaved.

Afterwards set the log level back to `error`.

## Commits

- Use Conventional Commits: `feat:`, `fix:`, `refactor:`, `build:`, `docs:`, `chore:`.
- The subject is lowercase, imperative and has no trailing period.
