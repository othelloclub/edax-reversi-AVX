cmake_minimum_required(VERSION 3.22.1)
project(edax C)

# ── edax engine source root (this repository's src/ and wasm/) ──
#   Override with -DEDAX_SRC_DIR=... from gradle/command line if needed.
#   android/ -> repository root is one level up.
if(NOT DEFINED EDAX_SRC_DIR)
  get_filename_component(EDAX_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()
set(SRC  "${EDAX_SRC_DIR}/src")
set(WASM "${EDAX_SRC_DIR}/wasm")

if(NOT EXISTS "${SRC}/search.c")
  message(FATAL_ERROR "edax source not found at ${SRC} (set -DEDAX_SRC_DIR=)")
endif()

# Same source set as ios/build_ios.sh + wasm/bridge.c + wasm/shim_log.c.
#  - shim_main.c excluded (main() clash).
#  - ui.c / nboard.c excluded: CLI/GUI dispatchers that pull in uncompiled CLI
#    implementations (edax.c/main.c); a shared .so must resolve every reference
#    (a static .a like iOS dead-strips them). usage() is stubbed in edax_jni.c.
set(EDAX_SRC_NAMES
    base bit board move game search book eval opening hash util
    options play root endgame midgame perft stats event ybwc)

set(EDAX_SOURCES "")
foreach(s ${EDAX_SRC_NAMES})
  list(APPEND EDAX_SOURCES "${SRC}/${s}.c")
endforeach()
list(APPEND EDAX_SOURCES "${WASM}/bridge.c" "${WASM}/shim_log.c")

# JNI bridge (this directory).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/edax_jni.c")
  list(APPEND EDAX_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/edax_jni.c")
endif()

add_library(edax SHARED ${EDAX_SOURCES})
target_include_directories(edax PRIVATE "${SRC}")

target_compile_options(edax PRIVATE
    -O3 -fvisibility=hidden -std=gnu11
    # NDK's flags.cmake injects -DANDROID into every compile. board.c / eval.c
    # exclude the NEON paths (board_sse.c / eval_sse.c) when ANDROID is defined,
    # leaving get_moves / eval_update_sse etc. undefined for a shared lib. Undo it
    # here (target options come after the toolchain CMAKE_C_FLAGS): arm64 then takes
    # the __ARM_NEON path (identical to iOS), x86_64 the hasSSE2 path.
    -UANDROID)

target_compile_definitions(edax PRIVATE
    NDEBUG
    UNICODE=1
    HAS_CPU_64
    MOVE_GENERATOR=MOVE_GENERATOR_BITSCAN
    COUNT_LAST_FLIP=COUNT_LAST_FLIP_BITSCAN
    __unix__=1)       # Bionic does not define __unix__; enables util's unix thread/clock paths.

# arm64-v8a: __aarch64__ enables HAS_CPU_64 / __ARM_NEON automatically.
find_library(log-lib log)
target_link_libraries(edax ${log-lib})
