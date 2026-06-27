#!/usr/bin/env bash
set -euo pipefail
# set -x      # 디버그 출력
cd "$(dirname "$0")"

EMCC=emcc
# EMCC_DEBUG=1 bash build_s.sh

# 1) 공통 컴파일 플래그 (싱글스레드)
#  - MOVE_GENERATOR/COUNT_LAST_FLIP 을 BITSCAN 으로 강제. 측정 결과 wasm 에서 BITSCAN(하드웨어
#    ctz/clz → i64.ctz/i64.clz 단일명령)이 가장 빠르다: 26빈칸 solve 기준 BITSCAN 80s vs
#    기본 MOVE_GENERATOR_32 116s vs SSE/SIMD128 116s(이득 0). settings.h 의 wasm 자동선택은
#    MOVE_GENERATOR_32(느림)로 떨어지므로 여기서 명시 강제한다. SIMD/SSE 플래그는 쓰지 않는다
#    (순수 스칼라 → 전 브라우저 호환, SIMD 감지/듀얼바이너리 불필요).
CFLAGS=(
  -O3 -DNDEBUG -DEMSCRIPTEN -DUNICODE=1
  -DMOVE_GENERATOR=MOVE_GENERATOR_BITSCAN -DCOUNT_LAST_FLIP=COUNT_LAST_FLIP_BITSCAN
  -I../src
  -s USE_ZLIB=0
  -fno-exceptions
)

# 2) 링크/런타임 플래그
#    - ocviewer 기본 런타임 산출물
#    - single-thread / native analyze export 포함
LDFLAGS=(
  -s ERROR_ON_UNDEFINED_SYMBOLS=0
  -s INITIAL_MEMORY=134217728      # 128MB. 측정: book-less 워커가 26빈칸 level-60 solve+전체 analyze 후 ~114MB로 정착(해시 고정할당). 128MB면 평상시 grow 0. (구 512MB는 ~4배 과대) book 적재 앱 빌드는 growth로 커버, 필요시 상향.
  -s ALLOW_MEMORY_GROWTH=1         # allow growth (book 적재/예외적 깊은 탐색 시 1GB까지)
  -s MAXIMUM_MEMORY=1073741824     # cap growth at 1GB
  -s STACK_SIZE=16777216
  -s EXIT_RUNTIME=0
  -s MODULARIZE=1
  -s EXPORT_ES6=1
  -s ENVIRONMENT=web,worker
  -s FORCE_FILESYSTEM=1
  -s FILESYSTEM=1
  -lidbfs.js                 # IDBFS 파일시스템 포함
  #--preload-file ../data/book.dat@/data/book.dat
  #--preload-file ../data/eval.dat@/data/eval.dat 
  -s WASM=1
  --closure 0

  # ASYNCIFY: mid-step cancel을 위해 emscripten_sleep로 JS event loop에 양도
  # ASYNCIFY_REMOVE로 yield call chain에 없는 함수들(file I/O, init, debug 출력)을 transform에서 제외
  # → prewarm 시간 단축. yield는 PVS_midgame/NWS_midgame/NWS_endgame에서만 발생.
  -s ASYNCIFY=1
  -s ASYNCIFY_STACK_SIZE=131072
  -s "ASYNCIFY_IMPORTS=[\"emscripten_sleep\"]"
  -s "ASYNCIFY_REMOVE=[\"fclose\",\"fflush\",\"fread\",\"fwrite\",\"fputc\",\"fputs\",\"puts\",\"putc\",\"putchar\",\"iprintf\",\"fiprintf\",\"vprintf\",\"vfprintf\",\"vfiprintf\",\"__vfprintf_internal\",\"printf_core\",\"__small_fprintf\",\"__small_vfprintf\",\"fmt_fp\",\"pop_arg\",\"out\",\"pad\",\"__fwritex\",\"__toread\",\"__overflow\",\"do_putc\",\"locking_putc\",\"do_putc_447\",\"locking_putc_448\",\"do_putc_453\",\"locking_putc_454\",\"bprint\",\"hash_print\",\"statistics_print\",\"search_observer\",\"result_print\",\"line_print\",\"board_print\",\"board_set\",\"show_current_move\",\"pv_debug\",\"is_pv_ok\",\"time_print\",\"book_init\",\"book_add\",\"book_add_board\",\"book_load\",\"book_get_game_stats\",\"book_get_line\",\"eval_open\",\"options_bound\",\"play_init\",\"play_force_init\",\"play_new\",\"play_move\",\"play_free\",\"parse_move\",\"play_user_move\",\"search_init\",\"search_free\",\"search_alloc_thread_hash\",\"hash_init\",\"hash_cleanup\",\"task_stack_init\",\"position_link\",\"position_search\",\"position_get_moves\",\"position_add_link\",\"flipsight_init\",\"flipsight_load_book\",\"flipsight_load_eval\",\"flipsight_shutdown\",\"solve_hint60_run_current_move_hook\",\"emit_solve_hint60_row_stdout\",\"emit_solve_hint60_done_stdout\",\"lower_ascii\"]"

  -s EXPORTED_FUNCTIONS=["_main","_flipsight_init","_flipsight_setboard","_flipsight_hint","_flipsight_set_threads","_malloc","_free","_flipsight_load_eval","_flipsight_load_book","_flipsight_last_score","_flipsight_set_depth","_flipsight_cancel_now","_flipsight_analyze_game","_flipsight_analyze_game_from_board","_flipsight_search_stream_prepare","_flipsight_search_stream_step","_flipsight_search_stream_finish","_flipsight_search_stream_run","_flipsight_search_stream_run_filtered"]
  -s EXPORTED_RUNTIME_METHODS=["cwrap","ccall","lengthBytesUTF8","stringToUTF8","FS","wasmMemory","HEAP8","HEAPU8","HEAP32","HEAPU32","HEAPF32","HEAPF64"]
)

# 3) 대상 소스
SRC_COMMON=(
  ../src/base.c
  ../src/bit.c
  ../src/board.c
  ../src/move.c
  ../src/game.c
  ../src/search.c
  ../src/book.c
  ../src/eval.c
  ../src/opening.c
  ../src/hash.c
  ../src/nboard.c
  ../src/util.c
  ../src/options.c
  ../src/ui.c
  ../src/play.c
  ../src/root.c
  ../src/endgame.c
  ../src/midgame.c
  ../src/perft.c
  ../src/stats.c
  ../src/event.c
  ../src/ybwc.c
  ./bridge.c
  ./shim_log.c
  ./shim_main.c
)

echo "[1/2] compile .o"
objs=()
for f in "${SRC_COMMON[@]}"; do
  o="${f##*/}"; o="${o%.c}.o"
  "$EMCC" "${CFLAGS[@]}" -c "$f" -o "$o"
  objs+=("$o")
done

echo "[2/2] link flipsight_s.{js,wasm}"
"$EMCC" "${objs[@]}" "${LDFLAGS[@]}" -o flipsight_s.js

echo "OK."
echo " - 산출물: wasm/flipsight_s.js (ESM), wasm/flipsight_s.wasm"
#echo " - book.dat 번들됨 (/data/book.dat)"
