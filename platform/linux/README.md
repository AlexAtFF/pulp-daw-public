# PULP on Linux

Builds PULP on Linux from the same sources. It adds `build.sh` and this folder, and changes no existing file.

## Build

You need CMake 3.24 or later, Ninja and GCC 11 or later. Ubuntu 22.04's own CMake is 3.22, so take a newer one with
`pip install --user cmake`. Then:

```
sudo apt install ninja-build g++ libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev libxi-dev libxinerama-dev libxrandr-dev libxcursor-dev libxcomposite-dev libxrender-dev
./build.sh          # everything: builds, runs the tests, puts the program in dist/PULP-linux
./build.sh quick    # engine, renderer and tests only (no JUCE, no window, none of the packages above but g++)
./build.sh app      # the program and the renderer only
```

Run `dist/PULP-linux/PULP`, or render a song without a window: `dist/PULP-linux/pulp_render song.pulp out.wav`.

## How it works

`build.sh` gives CMake `platform/linux/toolchain.cmake`, which:

- includes `pulp_compat.h` in every C++ file. It holds the three Microsoft C-runtime calls the engine makes (`_wfopen`,
  `_fseeki64`, `_ftelli64`), for GCC, and a stand-in for `juce::WindowsRegistry`, the Windows-only JUCE class that one
  menu entry of the program uses;
- adds `platform/linux/include` to the include path. Its small `windows.h` gives the MP3 damage test the Win32 calls
  it makes: `VirtualAlloc`, `VirtualProtect` and `VirtualFree` on `mmap` and `mprotect`, and `__try` / `__except`
  as a `SIGSEGV` guard.

The rest of PULP's Windows-only code already sits behind `#if JUCE_WINDOWS`, and JUCE plays through ALSA on Linux.

## Tested

On Ubuntu 22.04 (GCC 11.4, CMake 3.26, Ninja 1.10), 10 Nov 2026:

- `./build.sh` builds everything, the test suite passes (559 tests, 316,812 checks, 0 failed), and the factory
  library builds (282 recordings fetched, every checksum good).
- The program, driven by `--script`, opens an 84-second project of 16 SFZ instruments, plays it through ALSA with no
  underruns, and exports it. The export is identical to `pulp_render`'s, within 24-bit rounding.

## Known limits

- **File paths must be ASCII.** The engine opens files with `_wfopen(path.wstring().c_str(), ...)`, and GCC's
  `std::filesystem::path::wstring()` throws on any other character before the file is opened. The fix is a two-line
  change in `engine/src/Audio.cpp` (call `fopen(path.c_str(), ...)` when not on Windows), left out here so that no
  existing file changes.
- **"Open .pulp files with PULP"** in the File menu registers nothing on Linux (on Windows it writes the registry).
- **Some text names Windows.** The first-run audio setup looks for ASIO and Windows Audio, finds neither, and JUCE
  opens the default ALSA device instead (F10 picks another).
