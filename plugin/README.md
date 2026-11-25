# GASFREE Native DAW Plugin (CLAP / VST3)

Native C++20 DAW plugin wrapping the GASFREE modular synthesizer rack UI (`../app/`) in a cross-platform WebView (CHOC WebView: WebKitGTK on Linux, WKWebView on macOS, WebView2 on Windows) backed by a lock-free, real-time native C++ DSP engine.

## Architecture

- **Plugin standard:** Native **CLAP** (`src/clap_plugin.cpp`, `src/plugin_entry.cpp`), plus **VST3** (and optional macOS **AUv2**) via `third_party/clap-wrapper`.
- **DSP engine (`src/dsp/`):** Zero-allocation, lock-free real-time C++20 engine mirroring `../app/rack-worklet.js`:
  - Flattened sample-by-sample patch cable router (front CV ±5V + rear stereo audio line level)
  - Sample-accurate CLAP/MIDI note & controller routing + host tempo sync
  - Native partitioned FFT stereo convolution reverb (replaces WebAudio `ConvolverNode` 8-slot bridge)
  - Currently ported native DSP modules: `subharm`, `lfo`, `comp` (with oscilloscope), `delay`, `reverb` (12 WAV rooms + synthetic plate), `mixer`, `minimix`, `hyper` (remaining devices fall back to safe stereo passthrough while being ported)
- **GUI (`src/gui/` + `assets/plugin-bridge.js`):**
  - Serves `app/` via custom URI scheme (`gasfree://app.local` on Linux/macOS, `https://gasfree.local` on Windows), injecting `plugin-bridge.js` on the fly with zero edits to `app/`.
  - Linux (`webview_gui_linux.cpp`): `GtkPlug` (forced X11 backend via `GDK_BACKEND=x11` + `gdk_set_allowed_backends("x11")`, `WEBKIT_DISABLE_DMABUF_RENDERER=1` default for NVIDIA/XWayland) + WebKitGTK 4.1, pumped on CLAP's 16ms main-thread timer.
  - macOS (`webview_gui_mac.mm`): Cocoa `NSView` + WKWebView, with `app/` and `plugin-bridge.js` bundled in `Contents/Resources/`.
  - Windows (`webview_gui_win.cpp`): Win32 `HWND` + CHOC embedded WebView2 COM bindings (no external WebView2 SDK or `WebView2Loader.dll` required; uses the Edge WebView2 Runtime on Windows 10/11).

## Tested Environments

| Platform | Environment | What Was Verified | Not Yet Tested |
|---|---|---|---|
| **Linux x64** | Arch Linux (Omarchy, kernel 7.2, Hyprland/XWayland, NVIDIA, PipeWire/JACK 48 kHz) | `test_dsp`, `test_clap_host` (CLAP + VST3), and live DAW run in **Bitwig Studio 6.1** (embedded WebKitGTK front/rear UI, Subharm PLAY, live stereo meters) | Other Linux DAWs (Reaper, Ardour) |
| **macOS arm64** | Mac mini (Apple Silicon) | `test_dsp`, `test_clap_host` (`.clap` bundle), `test_gui_mac.mm` (WKWebView UI render, PLAY click, audio meter check), self-contained `Contents/Resources` bundling | Live macOS DAW run (Logic / Bitwig / Ableton) |
| **Windows x64** | Cross-compiled on Linux (`clang-cl 22.1.8` + `lld-link` + `xwin` Windows 10 SDK 10.0.26100, MSVC `/MT` static CRT) | `test_dsp.exe`, `test_clap_host.exe` (`GASFREE.clap` + `--vst3 GASFREE.vst3`), PE exports & OS-only DLL imports under **Wine 11.17** | Live WebView2 GUI rendering on a native Windows 10/11 DAW |

## Building (Linux)

```bash
# CLAP only (installs to ~/.clap/GASFREE.clap)
cmake -S plugin -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux

# CLAP + VST3 via clap-wrapper
cmake -S plugin -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGASFREE_BUILD_VST3=ON \
  -DCLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=ON \
  -DGASFREE_INSTALL_LOCAL=OFF
cmake --build build-linux --target gasfree_all test_dsp test_clap_host

./build-linux/test_dsp
./build-linux/test_clap_host ./build-linux/GASFREE.clap
./build-linux/test_clap_host --vst3 ./build-linux/Release/GASFREE.vst3/Contents/x86_64-linux/GASFREE.so
```

## Building (macOS)

```bash
cmake -S plugin -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGASFREE_BUILD_VST3=ON \
  -DCLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=ON \
  -DGASFREE_INSTALL_LOCAL=OFF
cmake --build build-macos --target gasfree_all test_dsp test_clap_host test_gui_mac

./build-macos/test_dsp
./build-macos/test_clap_host ./build-macos/GASFREE.clap
./build-macos/test_clap_host --vst3 ./build-macos/GASFREE.vst3/Contents/MacOS/GASFREE
```

## Cross-Compiling for Windows x64 (from Linux)

Windows x64 builds use `clang-cl` + `lld-link` + an `xwin` MSVC CRT / Windows SDK sysroot via [`cmake/toolchain-win64-clang-cl.cmake`](cmake/toolchain-win64-clang-cl.cmake). Output DLLs link the MSVC runtime statically (`/MT`) and depend only on built-in Windows OS DLLs (`KERNEL32.dll`, `USER32.dll`, `SHELL32.dll`, `ole32.dll`).

### 1. One-time prerequisites

- `clang-cl`, `llvm-lib`, `llvm-rc`, `llvm-mt`
- `lld-link` (system `lld` or auto-detected from `~/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/lld-link`)
- [`xwin`](https://github.com/Jake-Shadle/xwin/releases) to splat the x64 MSVC CRT & Windows SDK into `~/.cache/xwin`:

```bash
xwin --arch x86_64 --accept-license splat \
  --use-winsysroot-style --preserve-ms-arch-notation \
  --copy --output ~/.cache/xwin
```

### 2. Build Windows x64 CLAP + VST3

```bash
cmake -S plugin -B build-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/plugin/cmake/toolchain-win64-clang-cl.cmake" \
  -DGASFREE_BUILD_VST3=ON \
  -DCLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=ON \
  -DCPM_SOURCE_CACHE="$HOME/.cache/cpm" \
  -DGASFREE_INSTALL_LOCAL=OFF
cmake --build build-win64 --target gasfree_all test_clap_host test_dsp
```

### 3. Smoke-testing Windows builds under Wine

```bash
WINEDEBUG=-all wine build-win64/test_dsp.exe
WINEDEBUG=-all wine build-win64/test_clap_host.exe build-win64/CLAP/GASFREE.clap
WINEDEBUG=-all wine build-win64/test_clap_host.exe --vst3 build-win64/GASFREE.vst3
```

## Runtime Asset Discovery

By default the plugin loads UI/impulse assets from `../app` (relative to `plugin/` at build time). Override at runtime with `GASFREE_APP_DIR`.

When deployed from a portable release archive onto another machine, `ResourceProvider` automatically searches beside the plugin binary (`./app`, `../Resources/app`, `../../Resources/app`) for `app/` and `plugin-bridge.js`.
