// wasm/bridge.c (수정본)
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <emscripten/emscripten.h>

#include "../src/base.h"
#include "../src/bit.h"       // ← bit_init
#include "../src/board.h"     // ← edge_stability_init
#include "../src/stats.h"     // ← statistics_init
#include "../src/search.h"    // ← search_global_init
#include "../src/options.h"   // ← 전역 options
#include "../src/move.h"
#include "../src/const.h"
#include "../src/play.h"
#include "../src/book.h"      // ← Book 타입만 초기화(파일 로드 X)
#include "../src/eval.h"
#include "../src/util.h"

// 전역
static Play g_play[1];
static Book g_book[1];          // play_book_alternative 사용을 위해 실제 Book 인스턴스
static char g_bestmove[8];
static int  g_lastscore = 0;        // ★ 마지막 힌트 스코어 저장
static int  g_analyze_ply_offset = 0;

// 진행 중(혹은 직전에) 사용한 마스터 Search 포인터를 보관
static Search *g_current_master = NULL;

static void edax_register_master(Search *master) {  // 내부 전용
    g_current_master = master;
}

static void edax_clear_master(void) {               // 내부 전용
    g_current_master = NULL;
}

EMSCRIPTEN_KEEPALIVE                                  // JS에서 호출할 공개 엔트리
void flipsight_cancel_now(void) {
    if (g_current_master) {
        search_stop_all(g_current_master, STOP_ON_DEMAND);
    }
}

// ASYNCIFY: search hot path에서 호출되어 JS event loop로 제어 양도.
// 그동안 worker.onmessage가 발화되어 cancel 메시지 처리 → edax_cancel_now()까지 수행됨.
// 호출 빈도 제어는 호출자 측에서 search->n_nodes 마스킹으로 처리.
EMSCRIPTEN_KEEPALIVE
void edax_yield_to_host(void) {
    emscripten_sleep(0);
}

EMSCRIPTEN_KEEPALIVE
int flipsight_shutdown(void) {
    // 탐색 중단 루틴이 공개되어 있지 않으므로 직접 호출 제거
    // (play_free가 내부 리소스를 정리합니다)
    play_free(g_play);

    // 전역 리소스 정리
    eval_close();
    options_free();
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_load_eval(const char* path) {
    // 문자열 복제/해제 없이 그대로 사용
    options.eval_file = (char*) path;   // 예: "/data/eval.dat"
    eval_close();                       // 혹시 열려있던 거 정리
    eval_open(options.eval_file);       // 로드
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_load_book(const char* path) {
    // play_init 이후 g_play->book 사용 가능
    // book_init/book_load을 중복으로 부르지 말고, 이미 있는 책 객체에 로드
    options.book_file = (char*) path;   // 예: "/data/book.dat"
    book_load(g_play->book, options.book_file);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_init(void) {
    // CLI와 맞춘 공용 초기화
    bit_init();
    edge_stability_init();
    statistics_init();

    // 기본 옵션(필요한 만큼만 손봄)
    options.verbosity = 0;
    //options.depth = 20;
    //options.play_type = EDAX_FIXED_LEVEL;  // 시간/레벨 모드 막고 “깊이 고정”으로
    options.book_allowed = true;
    // 여기선 기본값만 넣어두고, 실제 파일 경로는 JS에서 edax_load_* 로 주입
    options.eval_file = "data/eval.dat";
    options.book_file = "data/book.dat";
    options_bound();

    // ★ 여기서 시간 기반 모드로 설정
    //options.play_type = EDAX_TIME_PER_MOVE;
    //search_set_move_time(&g_play->search, 3000);  // ← 3000ms = 3초


    // eval: 만약 JS에서 미리 edax_load_eval을 안 불렀다면 기본 경로로 오픈
    //eval_open(options.eval_file);

    search_global_init();

    // Book 초기화 후 Play에 연결 (play_book_alternative가 g_play->book을 직접 참조)
    book_init(g_book);
    play_init(g_play, g_book);

    return 0;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_setboard(const char* board_plus_turn) {
    // 예: "---------------------------OX------XO--------------------------- X"
    play_set_board(g_play, board_plus_turn);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void flipsight_set_depth(int d) {
    if (d < 0) d = 0;
    options.level = d;
    //options.play_type = EDAX_FIXED_LEVEL;
}

EMSCRIPTEN_KEEPALIVE
const char* flipsight_hint(void) {
    // 검색 포인터 등록 (취소 버튼 대응)
    edax_register_master(&g_play->search);

    // 2) 북에 없으면 검색 실행
    play_hint(g_play, 1);

    // 계산 끝났으니 포인터 해제
    edax_clear_master();

    // 3) PV 첫 수 = result->move 로 취급 (가장 안전)
    const Result* r = g_play->search.result;
    if (r && r->move != NOMOVE) {
        g_lastscore = r->score;
        int color = g_play->search.board.player;  // ← 현재 차례 색으로 통일
        move_to_string(r->move, color, g_bestmove);
        return g_bestmove;
    }

    // (선택) 정말로 PV 배열의 0번을 쓰고 싶다면, 헤더 확인 후 아래처럼:
    // if (r && r->pv.n_moves > 0) {
    //     int color = board_count_empties(&g_play->search.board) & 1;
    //     move_to_string(r->pv.move[0], color, g_bestmove);
    //     return g_bestmove;
    // }

    g_bestmove[0] = '\0';
    return g_bestmove;
}

// JS에서 기기 코어수에 맞춰 조정
EMSCRIPTEN_KEEPALIVE
void flipsight_set_threads(int n) {
        if (n < 1) n = 1;
    if (n > 64) n = 64;
    options.n_task = n;        
}

EMSCRIPTEN_KEEPALIVE
int flipsight_last_score(void) {          // ★ JS에서 불러갈 스코어
    return g_lastscore;
}

static int g_analyze_emitted_count = 0;
static int g_solve_hint60_emitted_count = 0;
static int g_solve_hint60_active = 0;
static int g_solve_hint60_done_emitted = 0;

static void lower_ascii(char* s) {
    if (!s) return;
    while (*s) {
        *s = (char)tolower((unsigned char)*s);
        ++s;
    }
}

static void emit_analyze_row_stdout(const AnalyzeHookRow *row) {
    char played_move[8] = "";
    char best_move[8] = "";
    const char *source = (row->source == ANALYZE_SOURCE_BOOK) ? "book" : "search";
    const char turn_bw = (row->turn == BLACK) ? 'B' : 'W';

    if (row->played_move != NOMOVE) {
        move_to_string(row->played_move, row->turn, played_move);
        lower_ascii(played_move);
    }
    if (row->best_move != NOMOVE) {
        move_to_string(row->best_move, row->turn, best_move);
        lower_ascii(best_move);
    }

    ++g_analyze_emitted_count;
    printf("ANALYZE_ROW\t{\"ply\":%d,\"turnBW\":\"%c\",\"playedMove\":\"%s\",\"playedScore\":%d,\"bestMove\":\"%s\",\"bestScore\":%d,\"legalCount\":%d,\"depth\":%d,\"percent\":%d,\"source\":\"%s\"}\n",
        row->ply + g_analyze_ply_offset + 1, turn_bw, played_move, row->played_score, best_move, row->best_score, row->legal_count, row->depth, row->percent, source);
    fflush(stdout);
}

static int solve_hint60_percent(int selectivity) {
    if (selectivity >= 0 && selectivity <= NO_SELECTIVITY) {
        return selectivity_table[selectivity].percent;
    }
    return 0;
}

static void emit_solve_hint60_row_stdout(int move, int score, int depth, int selectivity, int from_book) {
    char move_str[8] = "";
    const char *source = from_book ? "book" : "search";

    if (move != NOMOVE) {
        move_to_string(move, g_play->player, move_str);
        lower_ascii(move_str);
    }

    ++g_solve_hint60_emitted_count;
    printf("SOLVE_ROW\t{\"move\":\"%s\",\"score\":%d,\"depth\":%d,\"selectivity\":%d,\"percent\":%d,\"source\":\"%s\"}\n",
        move_str,
        score,
        depth,
        selectivity,
        solve_hint60_percent(selectivity),
        source);
    fflush(stdout);
}

static void emit_solve_hint60_done_stdout(Search *search) {
    if (g_solve_hint60_done_emitted) return;
    g_solve_hint60_done_emitted = 1;
    printf("SOLVE_DONE\t{\"count\":%d,\"stopped\":%s}\n",
        g_solve_hint60_emitted_count,
        (search && search->stop == STOP_ON_DEMAND) ? "true" : "false");
    fflush(stdout);
}

static void close_solve_hint60_stream(void) {
    if (!g_solve_hint60_active) return;
    g_play->search.options.multipv_depth = MULTIPV_DEPTH;
    edax_clear_master();
    g_play->state = IS_WAITING;
    g_solve_hint60_active = 0;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_search_stream_prepare(const char* board_plus_turn, int depth) {
    Search *search = &g_play->search;

    if (!board_plus_turn) return -1;

    close_solve_hint60_stream();
    g_solve_hint60_emitted_count = 0;
    g_solve_hint60_done_emitted = 0;
    if (depth < 0) depth = 0;
    options.level = depth;

    play_set_board(g_play, board_plus_turn);
    if (play_is_game_over(g_play)) {
        emit_solve_hint60_done_stdout(search);
        return 0;
    }

    play_stop_pondering(g_play);
    g_play->state = IS_THINKING;
    search->options.verbosity = 0;
    search_set_board(search, &g_play->board, g_play->player);
    search_set_level(search, options.level, search->eval.n_empties);
    search->stop = STOP_END;
    edax_register_master(search);
    g_solve_hint60_active = 1;
    return search->movelist.n_moves;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_search_stream_step(void) {
    Search *search = &g_play->search;
    MoveList book_moves;
    Move *m;

    if (!g_solve_hint60_active) return 0;

    if (search->stop == STOP_ON_DEMAND || movelist_is_empty(&search->movelist)) {
        emit_solve_hint60_done_stdout(search);
        close_solve_hint60_stream();
        return 0;
    }

    if (options.book_allowed && book_get_moves(g_play->book, &g_play->board, &book_moves)) {
        foreach_move (m, book_moves) {
            if (search->stop == STOP_ON_DEMAND) {
                emit_solve_hint60_done_stdout(search);
                close_solve_hint60_stream();
                return 0;
            }
            if (movelist_exclude(&search->movelist, m->x) != NULL) {
                emit_solve_hint60_row_stdout(m->x, m->score, 0, 0, 1);
                return 1;
            }
        }
    }

    if (movelist_is_empty(&search->movelist)) {
        emit_solve_hint60_done_stdout(search);
        close_solve_hint60_stream();
        return 0;
    }

    if (options.play_type == EDAX_TIME_PER_MOVE) search_set_move_time(search, options.time);
    else search_set_game_time(search, g_play->time[g_play->player].left);

    search_run(search);

    if (search->stop != STOP_END) {
        emit_solve_hint60_done_stdout(search);
        close_solve_hint60_stream();
        return 0;
    }

    if (search->result && search->result->move != NOMOVE) {
        emit_solve_hint60_row_stdout(
            search->result->move,
            search->result->score,
            search->result->depth,
            search->result->selectivity,
            0
        );
        movelist_exclude(&search->movelist, search->result->move);
        return 1;
    }

    emit_solve_hint60_done_stdout(search);
    close_solve_hint60_stream();
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_search_stream_finish(void) {
    Search *search = &g_play->search;
    emit_solve_hint60_done_stdout(search);
    close_solve_hint60_stream();
    return g_solve_hint60_emitted_count;
}

// ── Native-progressive implementation ──────────────────────────────────────
// Single search_run with iterative deepening inside Edax.
// Root move rows are emitted as each move finishes at each depth.

static void solve_hint60_run_observer(Result *result) {
    (void)result;
    edax_yield_to_host();
}

static void solve_hint60_run_current_move_hook(Search *s, Move *m) {
    char mv[8] = "";
    int percent = solve_hint60_percent(s ? s->selectivity : NO_SELECTIVITY);
    if (m->x != NOMOVE) {
        move_to_string(m->x, s->player, mv);
        lower_ascii(mv);
    }
    printf("SOLVE_PROGRESS\t{\"depth\":%d,\"percent\":%d,\"currentMove\":\"%s\"}\n", s->depth, percent, mv);
    fflush(stdout);
}

static void solve_hint60_run_current_move_result_hook(Search *s, Move *m, int depth, int selectivity) {
    if (!s || !m || m->x == NOMOVE) return;
    emit_solve_hint60_row_stdout(m->x, m->score, depth, selectivity, 0);
    edax_yield_to_host();
}

static void parse_only_moves_csv(const char *only_moves_csv, int allowed[64]) {
    const char *p;
    memset(allowed, 0, sizeof(int) * 64);
    if (!only_moves_csv) return;

    for (p = only_moves_csv; *p; ) {
        if (isalpha((unsigned char)p[0]) && isdigit((unsigned char)p[1])) {
            char mv[3];
            int x;
            mv[0] = (char)tolower((unsigned char)p[0]);
            mv[1] = p[1];
            mv[2] = '\0';
            x = string_to_coordinate(mv);
            if (A1 <= x && x <= H8) allowed[x] = 1;
            p += 2;
        } else {
            ++p;
        }
    }
}

static void filter_search_movelist(Search *search, const int allowed[64]) {
    Move *m;
    int remove[64];
    int n_remove = 0;
    int i;

    foreach_move (m, search->movelist) {
        if (A1 <= m->x && m->x <= H8 && !allowed[m->x]) {
            remove[n_remove++] = m->x;
        }
    }
    for (i = 0; i < n_remove; ++i) {
        movelist_exclude(&search->movelist, remove[i]);
    }
}

static int flipsight_search_stream_run_impl(const char* board_plus_turn, int max_depth, const char* only_moves_csv) {
    Search *search = &g_play->search;
    MoveList book_moves;
    Move *m;
    int allowed[64];
    int has_only_filter = (only_moves_csv && only_moves_csv[0]);

    if (!board_plus_turn) return -1;

    // Reset any previous stream state.
    close_solve_hint60_stream();
    g_solve_hint60_emitted_count = 0;
    g_solve_hint60_done_emitted = 0;

    if (max_depth < 0) max_depth = 0;
    options.level = max_depth;

    play_set_board(g_play, board_plus_turn);
    if (play_is_game_over(g_play)) {
        emit_solve_hint60_done_stdout(search);
        return 0;
    }

    play_stop_pondering(g_play);
    g_play->state = IS_THINKING;

    search_set_board(search, &g_play->board, g_play->player);
    search_set_level(search, options.level, search->eval.n_empties);

    if (has_only_filter) {
        parse_only_moves_csv(only_moves_csv, allowed);
        filter_search_movelist(search, allowed);
    }

    // Emit book moves first, then exclude them from the movelist.
    if (options.book_allowed && book_get_moves(g_play->book, &g_play->board, &book_moves)) {
        foreach_move (m, book_moves) {
            if (movelist_exclude(&search->movelist, m->x) != NULL) {
                emit_solve_hint60_row_stdout(m->x, m->score, 0, 0, 1);
            }
        }
    }

    if (movelist_is_empty(&search->movelist)) {
        emit_solve_hint60_done_stdout(search);
        g_play->state = IS_WAITING;
        return g_solve_hint60_emitted_count;
    }

    // multipv_depth=60: endgame depth can exceed max_depth (e.g. 24 empties → depth=24>21).
    // Setting 60 ensures full-window is always used for all moves at every depth.
    search->options.multipv_depth = 60;
    // verbosity=2 keeps Edax's iterative deepening observer active for timing/yield only.
    search->options.verbosity = 2;
    search->observer = solve_hint60_run_observer;
    search->current_move_hook = solve_hint60_run_current_move_hook;
    search->current_move_result_hook = solve_hint60_run_current_move_result_hook;
    search->stop = STOP_END;

    edax_register_master(search);
    search_run(search);
    edax_clear_master();

    search->observer = NULL;
    search->current_move_hook = NULL;
    search->current_move_result_hook = NULL;
    search->options.multipv_depth = MULTIPV_DEPTH;
    search->options.verbosity = 0;

    emit_solve_hint60_done_stdout(search);
    g_play->state = IS_WAITING;
    return g_solve_hint60_emitted_count;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_search_stream_run(const char* board_plus_turn, int max_depth) {
    return flipsight_search_stream_run_impl(board_plus_turn, max_depth, NULL);
}

EMSCRIPTEN_KEEPALIVE
int flipsight_search_stream_run_filtered(const char* board_plus_turn, int max_depth, const char* only_moves_csv) {
    return flipsight_search_stream_run_impl(board_plus_turn, max_depth, only_moves_csv);
}

static int count_board_discs64(const char *board_plus_turn) {
    int i, count = 0;
    if (!board_plus_turn) return 0;
    for (i = 0; i < 64 && board_plus_turn[i] != '\0'; ++i) {
        switch (tolower((unsigned char)board_plus_turn[i])) {
        case 'b':
        case 'x':
        case '*':
        case 'o':
        case 'w':
            ++count;
            break;
        default:
            break;
        }
    }
    return count;
}

static int analyze_game_from_position(const char* board_plus_turn, const char* moves_str) {
    int len, k;

    if (!moves_str) return -1;

    g_analyze_emitted_count = 0;
    g_analyze_ply_offset = 0;
    if (board_plus_turn && strlen(board_plus_turn) >= 64) {
        const int discs = count_board_discs64(board_plus_turn);
        g_analyze_ply_offset = (discs >= 4) ? (discs - 4) : 0;
        play_set_board(g_play, board_plus_turn);
    } else {
        play_new(g_play);
        board_init(&g_play->initial_board);
        board_init(&g_play->board);
        g_play->initial_player = BLACK;
        g_play->player = BLACK;
        g_play->i_game = g_play->n_game = 0;
    }

    len = (int)strlen(moves_str);
    for (k = 0; k + 1 < len; k += 2) {
        char mv[3] = { moves_str[k], moves_str[k + 1], 0 };
        if (play_must_pass(g_play)) {
            play_move(g_play, PASS);
        }
        if (!play_user_move(g_play, mv)) break;
    }

    edax_register_master(&g_play->search);
    g_analyze_result_hook = emit_analyze_row_stdout;
    play_analyze(g_play, g_play->i_game);
    g_analyze_result_hook = NULL;
    edax_clear_master();
    g_analyze_ply_offset = 0;

    printf("ANALYZE_DONE\t{\"count\":%d,\"stopped\":%s}\n",
        g_analyze_emitted_count,
        (g_play->search.stop == STOP_ON_DEMAND) ? "true" : "false");
    fflush(stdout);
    return g_analyze_emitted_count;
}

EMSCRIPTEN_KEEPALIVE
int flipsight_analyze_game(const char* moves_str) {
    return analyze_game_from_position(NULL, moves_str);
}

EMSCRIPTEN_KEEPALIVE
int flipsight_analyze_game_from_board(const char* board_plus_turn, const char* moves_str) {
    return analyze_game_from_position(board_plus_turn, moves_str);
}
