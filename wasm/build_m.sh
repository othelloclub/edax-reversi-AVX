#!/usr/bin/env bash
set -euo pipefail
# set -x      # ← 추가: 어떤 명령을 실행 중인지 출력
cd "$(dirname "$0")"

EMCC=emcc
# EMCC_DEBUG=1 bash build.sh

# 1) 공통 컴파일 플래그 (멀티스레드 + 최적화)
CFLAGS=(
  -O3 -DNDEBUG -DEMSCRIPTEN -DUNICODE=1
  -I../src
  -s USE_ZLIB=0
  -fno-exceptions
  -pthread                  # ← 중요: 모든 TU 컴파일에 스레드 플래그
)

# 2) 링크/런타임 플래그 (배열로 안전하게, 512MB 고정, 멀티스레드 8개)
# 현재 loader는 FORCE_SINGLE_THREAD=true라 기본 경로에서 이 빌드를 쓰지 않는다.
# single build memory-growth policy와 별도로 유지한다.
LDFLAGS=(
  -s ERROR_ON_UNDEFINED_SYMBOLS=0
  -s INITIAL_MEMORY=536870912      # 512MB 고정
  -s ALLOW_MEMORY_GROWTH=0         # 고정 메모리
  -s USE_PTHREADS=1
  -s PTHREAD_POOL_SIZE=8
  -s STACK_SIZE=16777216
  -s PROXY_TO_PTHREAD=1         # ★ 추가: 런타임을 워커로 프록시
  -s EXIT_RUNTIME=0
  -s MODULARIZE=1
  -s EXPORT_ES6=1
  -s ENVIRONMENT=web,worker
  -s FORCE_FILESYSTEM=1
  -s FILESYSTEM=1
  -lidbfs.js                 # ← IDBFS 파일시스템 포함
  #--preload-file ../data/book.dat@/data/book.dat
  #--preload-file ../data/eval.dat@/data/eval.dat 
  -s WASM=1
  --closure 0
  -pthread

  # ASYNCIFY: mid-step cancel을 위해 emscripten_sleep로 JS event loop에 양도
  # build_s.sh와 동일한 yield/cancel 경로를 멀티스레드 빌드에서도 유지
  -s ASYNCIFY=1
  -s ASYNCIFY_STACK_SIZE=131072
  -s "ASYNCIFY_IMPORTS=[\"emscripten_sleep\"]"
  -s "ASYNCIFY_REMOVE=[\"fclose\",\"fflush\",\"fread\",\"fwrite\",\"fputc\",\"fputs\",\"puts\",\"putc\",\"putchar\",\"iprintf\",\"fiprintf\",\"vprintf\",\"vfprintf\",\"vfiprintf\",\"__vfprintf_internal\",\"printf_core\",\"__small_fprintf\",\"__small_vfprintf\",\"fmt_fp\",\"pop_arg\",\"out\",\"pad\",\"__fwritex\",\"__toread\",\"__overflow\",\"do_putc\",\"locking_putc\",\"do_putc_447\",\"locking_putc_448\",\"do_putc_453\",\"locking_putc_454\",\"bprint\",\"hash_print\",\"statistics_print\",\"search_observer\",\"result_print\",\"line_print\",\"board_print\",\"board_set\",\"show_current_move\",\"pv_debug\",\"is_pv_ok\",\"time_print\",\"book_init\",\"book_add\",\"book_add_board\",\"book_load\",\"book_get_game_stats\",\"book_get_line\",\"eval_open\",\"options_bound\",\"play_init\",\"play_force_init\",\"play_new\",\"play_move\",\"play_free\",\"parse_move\",\"play_user_move\",\"search_init\",\"search_free\",\"search_alloc_thread_hash\",\"hash_init\",\"hash_cleanup\",\"task_stack_init\",\"position_link\",\"position_search\",\"position_get_moves\",\"position_add_link\",\"flipsight_init\",\"flipsight_load_book\",\"flipsight_load_eval\",\"flipsight_shutdown\",\"solve_hint60_run_current_move_hook\",\"emit_solve_hint60_row_stdout\",\"emit_solve_hint60_done_stdout\",\"lower_ascii\"]"

  -s EXPORTED_FUNCTIONS=["_main","_flipsight_init","_flipsight_setboard","_flipsight_hint","_flipsight_set_threads","_malloc","_free","_flipsight_load_eval","_flipsight_load_book","_flipsight_last_score","_flipsight_set_depth","_flipsight_cancel_now","_flipsight_analyze_game","_flipsight_analyze_game_from_board","_flipsight_search_stream_prepare","_flipsight_search_stream_step","_flipsight_search_stream_finish","_flipsight_search_stream_run","_flipsight_search_stream_run_filtered"]
  -s EXPORTED_RUNTIME_METHODS=["cwrap","ccall","lengthBytesUTF8","PThread","stringToUTF8","FS","wasmMemory","HEAP8","HEAPU8","HEAP32","HEAPU32","HEAPF32","HEAPF64"]
  #-s EXPORTED_RUNTIME_METHODS=["cwrap","ccall","lengthBytesUTF8","stringToUTF8","FS","wasmMemory","HEAP8","HEAPU8","HEAP32","HEAPU32","HEAPF32","HEAPF64"]
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

echo "[2/2] link flipsight_m.{js,wasm}"
"$EMCC" "${objs[@]}" "${LDFLAGS[@]}" -o flipsight_m.js

echo "OK."
echo " - 산출물: wasm/flipsight_m.js (ESM), wasm/flipsight_m.wasm"
#echo " - book.dat 번들됨 (/data/book.dat)"
