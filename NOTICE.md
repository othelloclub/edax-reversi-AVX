# NOTICE — License & Modifications

This repository contains a **modified version of Edax**, a strong othello
program released under the **GNU General Public License v3 (GPLv3)**.

- **Original Edax** — by Richard Delorme:
  https://github.com/abulmo/edax-reversi
- **SSE/AVX fork (upstream of this repository)** — by Toshihiko Okuhara:
  https://github.com/okuhara/edax-reversi-AVX
- **License** — GNU General Public License v3 (see [`LICENSE`](LICENSE))

This software is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

## Modifications in this fork

This fork adds WebAssembly, iOS-native, and Android-native builds of the engine
plus a small endgame-search tuning.

- `src/search.c` — endgame selectivity table adjusted within the level ≤ 21
  range (25–30 empties).
- `src/util.c`, `src/util.h` — Apple-platform spinlock and worker QoS.
- `wasm/bridge.c` — engine entry points (shared by the WASM and native builds).
- `wasm/shim_log.c`, `wasm/shim_main.c` — WASM runtime shims.
- `wasm/build_s.sh` (single-thread), `wasm/build_m.sh` (multi-thread),
  `ios/build_ios.sh` (iOS static xcframework),
  `android/CMakeLists.txt` (Android NDK shared library) — build scripts.
- `android/edax_jni.c` — JNI bridge for the Android shared library.

All other source files are unchanged from the upstream Edax / AVX fork.
