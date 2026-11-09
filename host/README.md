# ataboy host tool (C)

`libataboy` (`include/ataboy.h`, `src/`) and the `ataboy` command-line tool
(`cli/`) for the ATAboy automation API ([docs/API.md](../docs/API.md)). The
protocol code is the firmware's own (`firmware/apiframe.c`, `crc32.c`,
`api_proto.c`, `json.c`), compiled in. The only other dependency is the
vendored cJSON in `third_party/`.

Prebuilt binaries come from CI (`.github/workflows/host.yml`): every run
uploads `ataboy-linux-x86_64`, `ataboy-linux-aarch64` (both fully static,
musl), `ataboy-windows-x86_64.exe` (static mingw-w64) and
`ataboy-macos-universal` (arm64 + x86_64) as workflow artifacts. A `v*` tag
attaches them, with the firmware UF2, to the GitHub release.

## Building

You need CMake 3.16 or later and a C11 compiler. Ninja is optional: leave out
`-G Ninja` to use make.

Linux (gcc or clang):

    cmake -S host -B host/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    ninja -C host/build                 # host/build/ataboy

macOS (Xcode command line tools). The universal binary:

    cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
    cmake --build host/build

Windows, native with [MSYS2](https://www.msys2.org/) (UCRT64 shell):

    pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
    cmake -S host -B host/build -G Ninja && ninja -C host/build

Options (all off by default):

| option | effect |
|---|---|
| `-DATABOY_SANITIZE=ON` | AddressSanitizer + UndefinedBehaviorSanitizer, abort on the first error |
| `-DATABOY_FUZZ=ON` | libFuzzer targets (clang only), with ASan + UBSan |
| `-DATABOY_STATIC=ON` | link `ataboy` statically (use musl on Linux: glibc's static NSS is unreliable) |

Warnings are errors (`-Werror`).

## Cross-compiling

Windows x86-64 from Linux (Debian/Ubuntu: `gcc-mingw-w64-x86-64`):

    cmake -S host -B host/build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
          -DCMAKE_BUILD_TYPE=Release -DATABOY_STATIC=ON
    ninja -C host/build-win             # host/build-win/ataboy.exe

The tests cross-build too, and run under Wine.

Static Linux binaries are built in an Alpine container, as CI does (use an
arm64 machine, or `--platform linux/arm64` with QEMU, for aarch64):

    docker run --rm -v "$PWD:/src" -w /src alpine:3.20 sh -c '
      apk add build-base cmake ninja linux-headers &&
      cmake -S host -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DATABOY_STATIC=ON &&
      ninja -C /tmp/b && cp /tmp/b/ataboy ataboy-linux-$(uname -m)'

## Tests

    cmake -S host -B host/build -G Ninja -DATABOY_SANITIZE=ON
    ninja -C host/build && ctest --test-dir host/build --output-on-failure

None of the tests needs an ATAboy.

* `test_lib`: JSON output and helpers.
* `test_engine`: the request engine (`device.c`) against `tests/fakefw.c`,
  a model of the firmware's request handling (signature check, extended
  header, run-once request ids through the firmware's own `rid_cache.c`).
  It covers fallback to basic headers, request ids, resends after empty or
  lost replies, reboot detection, malformed replies, JSON results, log
  paging and `config set` value parsing.
* `test_serial`: the serial transport, linked without the platform layer.
  The test provides a fake clock and a fake serial port. The far end of
  the port runs the firmware's frame parser in front of the same model.
  Faults are injected on the wire: junk, stale frames, corrupted or lost
  replies, corrupted requests.
* `fuzz_<name>_corpus`: each fuzz target replays its seed corpus (see
  below).

The firmware's own unit tests (`tests/*.c` at the top level) also run in CI
under the sanitizers. See the `linux-gcc` job for the commands.

CI runs every test under ASan + UBSan: gcc and clang on Linux, Apple clang
on macOS. It also runs the tests natively on Windows (MSYS2) and inside the
Alpine static builds.

## Fuzzing

Each `tests/fuzz_<name>.c` becomes a fuzz target automatically. Such a file
defines only `int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)`:
no `main()` and no `LLVMFuzzerInitialize`. Seed inputs go in
`tests/corpus/fuzz_<name>/`, one file per input.

* With `-DATABOY_FUZZ=ON` (clang), `fuzz_<name>` is a libFuzzer binary:

      cmake -S host -B host/build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DATABOY_FUZZ=ON
      ninja -C host/build-fuzz
      mkdir -p /tmp/corpus && cp host/tests/corpus/fuzz_json/* /tmp/corpus/
      host/build-fuzz/fuzz_json /tmp/corpus -max_total_time=60

* In any other build, `fuzz_<name>` is linked with
  `tests/fuzz_replay_main.c`. That driver feeds the target each file or
  directory given on its command line, so ctest replays the seeds on every
  compiler. To reproduce a crash found in CI, pass the crash file (from the
  `fuzz-crashes` artifact) to either binary.

CI fuzzes every target on every push. Each target gets up to 60 s, and all
targets share a 10-minute budget. The run fails on any crash and uploads
the crashing inputs.
