#!/usr/bin/env bash
# edax-reversi-AVX → iOS xcframework (device arm64 + simulator arm64), stock 종반, BITSCAN 최고성능.
#  - FLIPSIGHT_FAST_ENDGAME 미정의 → search.c stock(25-27:98%, 28-30:87%).
#  - MOVE_GENERATOR/COUNT_LAST_FLIP=BITSCAN (ARM64 최적). shim_main.c 제외(main 충돌), shim_log.c 포함.
#  - bridge.c 는 #ifdef __EMSCRIPTEN__ 가드로 네이티브 컴파일. os_unfair_lock + 워커 QoS(util.h/util.c).
#  - Intel Mac 시뮬이 필요하면 x86_64 슬라이스 추가 후 sim arm64 와 lipo 병합.
set -euo pipefail
cd "$(dirname "$0")"
SRC=../src
WASM=../wasm
SRCS_SRC=( base bit board move game search book eval opening hash nboard util
           options ui play root endgame midgame perft stats event ybwc )

build_slice() {  # $1=arch  $2=sdk  $3=target
  local OBJ="build/obj-$1-$2"; rm -rf "$OBJ"; mkdir -p "$OBJ"
  local SDKP; SDKP=$(xcrun --sdk "$2" --show-sdk-path)
  local CF=( -arch "$1" -target "$3" -isysroot "$SDKP"
             -O3 -DNDEBUG -DUNICODE=1 -DHAS_CPU_64
             -DMOVE_GENERATOR=MOVE_GENERATOR_BITSCAN -DCOUNT_LAST_FLIP=COUNT_LAST_FLIP_BITSCAN
             -I"$SRC" -fvisibility=hidden -std=gnu11 )
  local objs=()
  for s in "${SRCS_SRC[@]}"; do clang "${CF[@]}" -c "$SRC/$s.c" -o "$OBJ/$s.o"; objs+=("$OBJ/$s.o"); done
  clang "${CF[@]}" -c "$WASM/bridge.c"   -o "$OBJ/bridge.o";   objs+=("$OBJ/bridge.o")
  clang "${CF[@]}" -c "$WASM/shim_log.c" -o "$OBJ/shim_log.o"; objs+=("$OBJ/shim_log.o")
  xcrun --sdk "$2" libtool -static -o "build/libedax-$1-$2.a" "${objs[@]}"
  echo "  -> build/libedax-$1-$2.a"
}

echo "[1/3] device arm64";    build_slice arm64 iphoneos        arm64-apple-ios14.0
echo "[2/3] simulator arm64"; build_slice arm64 iphonesimulator arm64-apple-ios14.0-simulator

echo "[3/3] xcframework"
rm -rf build/libedax.xcframework
xcodebuild -create-xcframework \
  -library build/libedax-arm64-iphoneos.a \
  -library build/libedax-arm64-iphonesimulator.a \
  -output build/libedax.xcframework >/dev/null
echo "OK -> build/libedax.xcframework"
ls -1 build/libedax.xcframework
