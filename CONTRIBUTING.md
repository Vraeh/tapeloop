# Contributing

Tapeloop is maintained by one person in spare time, so please open an issue to discuss
a change before writing it. Bug reports with an OBS log attached are always welcome.

## Branches

The project follows git-flow:

- `dev` is where development happens. Pull requests target `dev`.
- `master` only receives release and hotfix branches, and every commit on it is a
  release.
- Work goes on a `feature/<name>` branch created from `dev`.

## Commits

Each commit is one logical change and must build on its own. Messages follow the same
convention as OBS Studio:

```
module: Short summary in the imperative

Explain what the change does and, above all, why. Wrap the body at 72
characters.
```

- The summary is at most 50 characters after the prefix, starts with a capital letter
  and has no trailing period.
- The prefix names the part of the project touched: `core`, `obs`, `ui`, `plugin`,
  `cmake`, `CI`, `github`, `docs`, `tests` or `data`. Separate several with commas.

Fix-up commits made during review are squashed into the commits they fix before the
pull request is merged.

## Code style

C++ follows the C++ section of the OBS Studio
[code style guide](https://github.com/obsproject/obs-studio/blob/master/CODESTYLE.md).
Formatting is checked in CI, so run the formatters before pushing:

```
./build-aux/run-clang-format
./build-aux/run-gersemi
```

They need the same versions CI uses: clang-format 19.1.1 and gersemi 0.21.0.

## Tests

The core library in `src/core` does not depend on OBS, so its tests build as a
standalone CMake project in `tests`, with Catch2 fetched at configure time. Each
platform has a workflow preset that configures, builds and runs them:

```
cd tests
cmake --workflow --preset linux-gcc
```

The presets are `linux-gcc`, `linux-clang-asan`, `linux-clang-tsan`, `windows-msvc` and
`macos`. CI runs all five on every pull request.

CI also runs the libFuzzer harnesses in `tests/fuzz` for a minute each. They need
LLVM Clang (Apple's Clang has no libFuzzer):

```
cd tests
cmake --preset linux-clang-fuzz
cmake --build --preset linux-clang-fuzz
mkdir -p ../build_tests/corpus
../build_tests/linux-clang-fuzz/fuzz/source-buffer-fuzz ../build_tests/corpus fuzz/corpus/source-buffer
```

libFuzzer writes new inputs into the first directory it is given, so keep the committed
corpus second. To reproduce a crash from the `fuzz-crashes` artifact of a CI run, pass
the file instead of the directories.

CI also runs clang-tidy over `src/core` with the checks in `.clang-tidy`, and any
warning fails the job. To run it locally with Clang 18:

```
cd tests
cmake --preset linux-clang-tidy
cd ..
clang-tidy -p build_tests/linux-clang-tidy src/core/*.cpp
```

## License

By contributing you agree that your contributions are licensed under the
GPL-2.0-or-later, the license of the project.
