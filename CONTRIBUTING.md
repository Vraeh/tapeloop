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

## FFmpeg

On Windows and Linux the plugin decodes with its own FFmpeg, linked statically: version
8.1.3 with only the H.264 and HEVC decoders and parsers (and the D3D11VA hwaccels on
Windows), under the LGPL. CMake builds it the first time it configures, with
`cmake/ffmpeg/BuildFFmpeg.cmake`, into `.deps/ffmpeg-<key>`, where the key follows that
script and the compiler, so a new version, option or compiler builds again. The build
takes about a minute on Linux and four on Windows, and needs nasm; on Windows it also
needs MSYS2 with make, nasm and diffutils (`pacman -S make nasm diffutils`) and uses the
Visual Studio, SDK and toolset the plugin builds with. FFmpeg's configure cannot build
under a path with spaces, so the checkout must not be in one. A Debug build on Windows
would link FFmpeg's release runtime (`-MD`) with the debug one, which MSVC warns about
(LNK4098) and warnings as errors would stop; it has not been tried, so build
RelWithDebInfo or Release. The tarball is pinned by SHA-256,
and its signature was checked against FFmpeg's release key when the version was pinned:

```
gpg --import ffmpeg-devel.asc   # https://ffmpeg.org/ffmpeg-devel.asc
gpg --verify ffmpeg-8.1.3.tar.xz.asc ffmpeg-8.1.3.tar.xz
# Primary key fingerprint: FCF9 86EA 15E6 E293 A564  4F10 B432 2F04 D676 58D8
```

macOS decodes with VideoToolbox and does not build FFmpeg.

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

The libobs glue is tested in `tests/obs` against a real libobs running headless: the
OBS sources pinned in `buildspec.json`, built without the frontend or the plugin set,
plus OBS's obs-x264 plugin, rendering with Mesa's software OpenGL under Xvfb. Building
libobs needs its Linux build dependencies, x264 and FFmpeg, and the tests need nasm for
the plugin's own FFmpeg; the `linux-obs` job in
`.github/workflows/run-tests.yaml` lists the Ubuntu packages. Then:

```
cmake -DPREFIX=$HOME/libobs -DWORK_DIR=$HOME/libobs-build -P tests/obs/BuildLibobs.cmake
cd tests/obs
export TAPELOOP_LIBOBS_PREFIX=$HOME/libobs
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run --auto-servernum cmake --workflow --preset linux-obs
```

libobs finds its data through the prefix it was built for, so build it where it will
stay. Each test starts libobs and shuts it down, and fails if libobs still holds memory
or had to free objects or views itself. CI builds libobs and the tests with ASan and
UBSan instead (`-DSANITIZE=ON` and the `linux-obs-asan` preset); `tests/obs/lsan.supp`
leaves out what Mesa allocates in the OpenGL driver and keeps until exit.

CI builds the core tests with the `linux-clang-coverage` preset, reports the line coverage
of `src/core` and fails below 90%.

CI also runs clang-tidy over `src/core` with the checks in `.clang-tidy`, and over the
core tests and the fuzz harnesses with only the rule that every branch has braces; any
warning fails the job. To run it locally with Clang 18:

```
cd tests
cmake --preset linux-clang-tidy
cd ..
clang-tidy -p build_tests/linux-clang-tidy src/core/*.cpp
clang-tidy -p build_tests/linux-clang-tidy --checks='-*,readability-braces-around-statements' \
  --header-filter='tests/core/' tests/core/*.cpp
clang-tidy --checks='-*,readability-braces-around-statements' --header-filter='tests/fuzz/' \
  tests/fuzz/*.cpp -- -std=c++20 -Isrc
```

## License

By contributing you agree that your contributions are licensed under the
GPL-2.0-or-later, the license of the project.
