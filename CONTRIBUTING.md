# Contributing

Thanks for helping! Good first places to look: [docs/ROADMAP.md](docs/ROADMAP.md),
[docs/MULTIPLAYER.md](docs/MULTIPLAYER.md) and the open issues.

## Ground rules
- **Never commit or attach game code or data.** That includes `gen/`, `gen_uw/`, build folders,
  Ghidra databases or decompiled output, extracted strings, screenshots of copyrighted content in
  bulk, and any `.exe`/`.dll`/`.dat` from the game. `.gitignore` covers the usual paths; please
  double-check `git status` before committing.
- Issues and PRs should describe behaviour, addresses and code, not paste disassembly listings of
  large functions.
- By contributing you agree that your contribution is licensed under the MIT license.

## Setup
```sh
xcode-select --install
brew install cmake ninja sdl3 python
./build.sh "/path/to/Populous The Beginning"      # first full build
```
Day-to-day:
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build poptb poptbuw
POPTB_GAME="/path/to/game" tools/run.sh 60        # run for 60 s
```
If you change `recomp/`, regenerate `gen/` (see README "How it works" and `build.sh`).

## Where things live
- New Win32 function → add `SHIM(Name, nargs)` in the matching `runtime/*.c` file.
- New COM method → the vtable tables at the bottom of `ddraw.c`, `dinput.c`, `dsound.c`.
- Recompiler instruction semantics → `recomp/gen.py` (`_gen`, `fpu`). Run `tools/fuzz.sh` after
  touching it; it must stay at "only `pushfd` differs".
- Code discovery problems (`dispatch: unknown target`) → `recomp/image.py`.

## Testing a change
1. `tools/fuzz.sh` if you touched the recompiler.
2. Intro movies, main menu, start the tutorial, run the Rolling Demo a few minutes:
   `tools/run.sh 180 --shot-dir /tmp/shots --shot-every 300 --script "17000:click:320,302"`.
3. If you touched audio: `POPTB_WAV=/tmp/out.raw tools/run.sh 40` and check the level is not silent.
4. Undiscovered Worlds: `POPTB_APP=PopulousUW tools/run.sh ...`.
See [docs/DEBUGGING.md](docs/DEBUGGING.md) for crash symbolication, tracing and replay tools.

## Pull requests
- One topic per PR; explain what you observed in the game and how you verified the fix.
- Keep the style of the surrounding code (C11, small helpers, comments explain *why*).
- Update `CHANGELOG.md` (Unreleased) and the relevant `docs/` file when you learn something new
  about the game: [docs/FINDINGS.md](docs/FINDINGS.md) is the shared memory of the project.

## Releases
1. Move the `[Unreleased]` entries of `CHANGELOG.md` into a `## [x.y.z] - date` section, written for
   players: these entries become the GitHub release notes (`tools/release_notes.sh x.y.z` shows them).
2. Set the version in `CMakeLists.txt`, commit, then push a `vx.y.z` tag. CI builds, signs, notarizes
   and publishes the release; it fails early if the changelog has no section for the version.
