/*
 * jni_bridge.c — Android JNI 브리지 for 네이티브 edax (bridge.c).
 *
 *  목적: bridge.c 의 flipsight_* C API 를 Kotlin EdaxPlugin 의 native 메서드로 노출하고,
 *        bridge.c 가 호출하는 4종 콜백(fs_row_cb / fs_solve_row_cb / fs_solve_progress_cb /
 *        fs_solve_done_cb)을 Kotlin 인스턴스 메서드로 다시 올려준다.
 *
 *  ★ 핵심 난점: 이 콜백들은 edax YBWC 워커 스레드(util.c thread_create 로 만든 pthread)에서
 *     호출될 수 있다. JNI 는 스레드별 JNIEnv* 가 필요하며, JavaVM 에 붙지 않은(native-only)
 *     스레드에서 JNIEnv 를 그냥 쓰면 즉시 크래시한다. 따라서:
 *       1) JNI_OnLoad 에서 JavaVM* 를 캐시한다(전역 1개).
 *       2) Kotlin EdaxPlugin 인스턴스를 NewGlobalRef 로 잡고, 콜백 대상 jmethodID 를 캐시한다.
 *       3) 콜백 진입 시 GetEnv 로 현재 스레드의 JNIEnv 를 얻고, 실패(미부착)면
 *          AttachCurrentThread 로 붙인다. edax 워커는 task_stack_init 에서 1회 생성되어
 *          솔브/분석 전 구간을 task_loop 로 살아있는 "풀 스레드"이므로, 매 콜백마다
 *          attach/detach 하면 비용이 크다 → "attach 후 그대로 두고", 그 스레드가 종료될 때
 *          pthread TLS 파괴자(thread_exit_detach)에서 DetachCurrentThread 로 정리한다.
 *          (메인/Kotlin 워커 큐 스레드처럼 원래 JVM 에 붙어있던 스레드는 우리가 attach 하지
 *           않았으므로 detach 도 하지 않는다 — did_attach 플래그로 구분.)
 *       4) const char* → NewStringUTF, int → jint 로 변환. 콜백 1회당 다수의 지역참조(jstring)가
 *          생기므로 PushLocalFrame/PopLocalFrame 로 프레임을 잡아 누수를 원천 차단한다.
 *
 *  스레드 안전: ctx 로 넘어오는 globalRef(EdaxPlugin) 와 jmethodID 들은 SetCallbacks 시점에
 *     1회 확정되어 콜백 구간 내내 불변이므로 추가 락 불필요. JavaVM* 도 OnLoad 1회 캐시.
 */

#include <jni.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <android/log.h>

#define LOG_TAG "EdaxJNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

/* ───── bridge.c 네이티브 콜백 타입 (bridge.c 의 #ifndef __EMSCRIPTEN__ 블록과 동일) ───── */
typedef void (*fs_row_cb)(void *ctx, int ply, char turnBW,
                          const char *playedMove, int playedScore,
                          const char *bestMove, int bestScore,
                          int legalCount, int depth, int percent,
                          const char *source);
typedef void (*fs_solve_row_cb)(void *ctx, const char *move, int score,
                                int depth, int selectivity, int percent, int fromBook);
typedef void (*fs_solve_progress_cb)(void *ctx, int depth, int percent, const char *currentMove);
typedef void (*fs_solve_done_cb)(void *ctx, int count, int stopped);

/* ───── bridge.c 가 export 하는 flipsight_* (KEEPALIVE) ───── */
extern int   flipsight_init(void);
extern int   flipsight_load_eval(const char *path);
extern void  flipsight_set_analyze_threads(int n);
extern void  flipsight_set_solve_threads(int n);
extern void  flipsight_set_analyze_depth(int d);
extern int   flipsight_analyze_game(const char *moves_str);
extern int   flipsight_analyze_game_from_board(const char *board_plus_turn, const char *moves_str);
extern void  flipsight_cancel_analyze(void);
extern int   flipsight_search_stream_run_filtered(const char *board_plus_turn, int max_depth, const char *only_moves_csv);
extern void  flipsight_cancel_solve(void);
extern long  flipsight_solve_mem_bytes(void);

extern void  flipsight_set_row_callback(fs_row_cb cb, void *ctx);
extern void  flipsight_set_solve_callbacks(fs_solve_row_cb row, fs_solve_progress_cb prog,
                                           fs_solve_done_cb done, void *ctx);

/* ── CLI 전용 stub ──────────────────────────────────────────────────────────
 * options.c 는 "help"/"?" 옵션 파싱 시 usage()(원래 main.c:44 `void usage(void)`)를 호출한다.
 * libedax.so 는 CLI 진입(main.c/ui.c)을 링크하지 않으므로(shared 라이브러리는 모든 심볼을
 * resolve 해야 함 — 정적 .a 인 iOS 는 미사용분이 dead-strip 되어 문제없지만 .so 는 아님)
 * 빈 stub 을 둔다. JS shim 은 옵션 파일을 읽지 않아 실제로 도달 불가능한 경로다. */
void usage(void) {}

/* ════════════════════════════════════════════════════════════════════════════
 *  전역 상태
 * ════════════════════════════════════════════════════════════════════════════ */
static JavaVM *g_vm = NULL;

/* Kotlin EdaxPlugin 인스턴스(GlobalRef) + 콜백 대상 메서드 ID.
 * SetCallbacks 에서 1회 확정. 콜백에 넘기는 ctx 는 g_plugin 그 자체(globalRef)이므로
 * 콜백 함수는 (jobject)ctx 를 바로 쓴다(추가 룩업 0). */
static jobject  g_plugin     = NULL;   /* NewGlobalRef(EdaxPlugin) */
static jmethodID g_mid_row   = NULL;   /* onAnalyzeRow(...)  */
static jmethodID g_mid_srow  = NULL;   /* onSolveRow(...)    */
static jmethodID g_mid_sprog = NULL;   /* onSolveProgress(...) */
static jmethodID g_mid_sdone = NULL;   /* onSolveDone(...)   */

/* 우리가 attach 한 스레드만 detach 하기 위한 TLS 키 */
static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

/* TLS 파괴자: 이 값이 비-NULL 이면 "우리가 이 스레드를 attach 했다"는 표시 →
 * 스레드 종료 시 자동 detach. (edax 워커 pthread 가 task_stack_free → join 으로
 * 끝날 때, 혹은 프로세스 정리 시 호출됨.) */
static void thread_exit_detach(void *value) {
    (void)value;
    if (g_vm) {
        (*g_vm)->DetachCurrentThread(g_vm);
    }
}

static void make_tls_key(void) {
    pthread_key_create(&g_tls_key, thread_exit_detach);
}

/* 현재 스레드의 JNIEnv 를 확보. 미부착이면 attach.
 * *out_did_attach 는 "이번 호출에서 새로 attach 했는지"가 아니라 "이 스레드를
 * 우리가 attach 해서 관리 중인지"를 나타낸다(즉시 detach 하지 않고 TLS 파괴자에 위임).
 * 반환: JNIEnv* (실패 시 NULL). */
static JNIEnv *acquire_env(void) {
    JNIEnv *env = NULL;
    if (!g_vm) return NULL;

    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_OK) {
        /* 이미 JVM 에 붙은 스레드(메인/Kotlin 큐). 우리가 붙인 게 아니므로 detach 책임 없음. */
        return env;
    }
    if (rc == JNI_EDETACHED) {
        /* native-only 워커 스레드 → 붙인다. 한 번만 키 생성. */
        pthread_once(&g_tls_once, make_tls_key);

#ifdef __ANDROID__
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) {
#else
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) != JNI_OK) {
#endif
            LOGE("AttachCurrentThread failed");
            return NULL;
        }
        /* TLS 에 마킹 → 이 스레드 종료 시 thread_exit_detach 가 detach.
         * 매 콜백마다 attach/detach 하지 않으므로(워커는 솔브 내내 살아있음) 비용 최소. */
        pthread_setspecific(g_tls_key, (void *)env);
        return env;
    }
    LOGE("GetEnv unexpected rc=%d", (int)rc);
    return NULL;
}

/* NewStringUTF 안전판: NULL/예외 시 NULL 반환(콜러는 그대로 jstring 인자로 넘김 → Kotlin 에서 nullable). */
static jstring new_utf(JNIEnv *env, const char *s) {
    if (!s) return NULL;
    jstring js = (*env)->NewStringUTF(env, s);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);   /* OOM 등 — 콜백 전체를 죽이진 않는다 */
        return NULL;
    }
    return js;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  bridge.c 가 호출하는 static C 콜백 (워커 스레드에서 진입 가능)
 *  ctx == g_plugin (GlobalRef). 매번 PushLocalFrame 으로 지역참조 누수 차단.
 * ════════════════════════════════════════════════════════════════════════════ */

static void cb_row(void *ctx, int ply, char turnBW,
                   const char *playedMove, int playedScore,
                   const char *bestMove, int bestScore,
                   int legalCount, int depth, int percent,
                   const char *source) {
    if (!ctx || !g_mid_row) return;
    JNIEnv *env = acquire_env();
    if (!env) return;

    /* 지역참조 최대 4개(playedMove/bestMove/source jstring + turnBW jstring) + 여유.
     * 프레임을 잡으면 PopLocalFrame 한 방에 전부 해제 → DeleteLocalRef 누락 위험 0. */
    if ((*env)->PushLocalFrame(env, 8) != JNI_OK) return;

    /* char(turnBW) 는 Kotlin Char(=UTF-16 code unit) 로 직결 가능하나, 'B'/'W' ASCII 라
     * jchar 로 바로 넘긴다(불필요한 String 할당 회피). */
    jchar j_turn = (jchar)(unsigned char)turnBW;
    jstring j_played = new_utf(env, playedMove);
    jstring j_best   = new_utf(env, bestMove);
    jstring j_src    = new_utf(env, source);

    (*env)->CallVoidMethod(env, (jobject)ctx, g_mid_row,
        (jint)ply, j_turn,
        j_played, (jint)playedScore,
        j_best,   (jint)bestScore,
        (jint)legalCount, (jint)depth, (jint)percent,
        j_src);

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);   /* logcat 에 스택 */
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
}

static void cb_solve_row(void *ctx, const char *move, int score,
                         int depth, int selectivity, int percent, int fromBook) {
    if (!ctx || !g_mid_srow) return;
    JNIEnv *env = acquire_env();
    if (!env) return;
    if ((*env)->PushLocalFrame(env, 4) != JNI_OK) return;

    jstring j_move = new_utf(env, move);
    (*env)->CallVoidMethod(env, (jobject)ctx, g_mid_srow,
        j_move, (jint)score, (jint)depth,
        (jint)selectivity, (jint)percent, (jint)fromBook);

    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    (*env)->PopLocalFrame(env, NULL);
}

static void cb_solve_progress(void *ctx, int depth, int percent, const char *currentMove) {
    if (!ctx || !g_mid_sprog) return;
    JNIEnv *env = acquire_env();
    if (!env) return;
    if ((*env)->PushLocalFrame(env, 4) != JNI_OK) return;

    jstring j_cur = new_utf(env, currentMove);
    (*env)->CallVoidMethod(env, (jobject)ctx, g_mid_sprog,
        (jint)depth, (jint)percent, j_cur);

    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    (*env)->PopLocalFrame(env, NULL);
}

static void cb_solve_done(void *ctx, int count, int stopped) {
    if (!ctx || !g_mid_sdone) return;
    JNIEnv *env = acquire_env();
    if (!env) return;
    /* jstring 없음 → 프레임 불필요하나, 일관성 위해 가볍게 보호. */
    (*env)->CallVoidMethod(env, (jobject)ctx, g_mid_sdone,
        (jint)count, (jint)stopped);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  JNI_OnLoad : JavaVM 캐시
 * ════════════════════════════════════════════════════════════════════════════ */
JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    /* 키는 첫 워커 attach 시 pthread_once 로 생성(여기서 미리 만들어도 무방). */
    pthread_once(&g_tls_once, make_tls_key);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNI_OnUnload(JavaVM *vm, void *reserved) {
    (void)reserved;
    /* 콜백 등록 해제 + globalRef 정리. (보통 프로세스 종료까지 안 옴) */
    flipsight_set_row_callback(NULL, NULL);
    flipsight_set_solve_callbacks(NULL, NULL, NULL, NULL);
    JNIEnv *env = NULL;
    if (vm && (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK && g_plugin) {
        (*env)->DeleteGlobalRef(env, g_plugin);
    }
    g_plugin = NULL;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  JNI export : Java_com_othelloclub_flipsight_EdaxPlugin_n*
 *  (Kotlin: external fun nInit(): Int 등. 패키지 com.othelloclub.flipsight,
 *   클래스 EdaxPlugin.)
 * ════════════════════════════════════════════════════════════════════════════ */
#define JNI_FN(name) Java_com_othelloclub_flipsight_EdaxPlugin_##name

/* 콜백 등록: thiz(EdaxPlugin) 를 GlobalRef 로 잡고, 4개 메서드 ID 를 캐시한 뒤
 * bridge.c 의 set_*_callbacks 에 우리 static C 콜백 + ctx(=globalRef) 로 등록.
 * 시그니처는 Kotlin 메서드와 정확히 일치해야 한다(아래 § Kotlin 메서드 시그니처). */
JNIEXPORT void JNICALL
JNI_FN(nSetCallbacks)(JNIEnv *env, jobject thiz) {
    /* 재등록 대비: 기존 globalRef 해제 */
    if (g_plugin) {
        (*env)->DeleteGlobalRef(env, g_plugin);
        g_plugin = NULL;
    }
    g_plugin = (*env)->NewGlobalRef(env, thiz);

    jclass cls = (*env)->GetObjectClass(env, thiz);
    /* analyze 행: (ply:Int, turnBW:Char, playedMove:String?, playedScore:Int,
     *              bestMove:String?, bestScore:Int, legalCount:Int, depth:Int,
     *              percent:Int, source:String?) -> Unit */
    g_mid_row   = (*env)->GetMethodID(env, cls, "onAnalyzeRow",
        "(ICLjava/lang/String;ILjava/lang/String;IIIILjava/lang/String;)V");
    /* solve 행: (move:String?, score:Int, depth:Int, selectivity:Int, percent:Int, fromBook:Int) -> Unit */
    g_mid_srow  = (*env)->GetMethodID(env, cls, "onSolveRow",
        "(Ljava/lang/String;IIIII)V");
    /* solve 진행: (depth:Int, percent:Int, currentMove:String?) -> Unit */
    g_mid_sprog = (*env)->GetMethodID(env, cls, "onSolveProgress",
        "(IILjava/lang/String;)V");
    /* solve 완료: (count:Int, stopped:Int) -> Unit */
    g_mid_sdone = (*env)->GetMethodID(env, cls, "onSolveDone",
        "(II)V");

    (*env)->DeleteLocalRef(env, cls);

    if (!g_mid_row || !g_mid_srow || !g_mid_sprog || !g_mid_sdone) {
        LOGE("GetMethodID failed: row=%p srow=%p sprog=%p sdone=%p",
             (void*)g_mid_row, (void*)g_mid_srow, (void*)g_mid_sprog, (void*)g_mid_sdone);
        /* 예외가 걸려 있을 수 있으니 정리 */
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return;
    }

    /* bridge.c 에 등록: ctx == g_plugin(GlobalRef). 콜백 내부에서 (jobject)ctx 로 바로 사용. */
    flipsight_set_row_callback(cb_row, g_plugin);
    flipsight_set_solve_callbacks(cb_solve_row, cb_solve_progress, cb_solve_done, g_plugin);
}

JNIEXPORT jint JNICALL
JNI_FN(nInit)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)flipsight_init();
}

JNIEXPORT jint JNICALL
JNI_FN(nLoadEval)(JNIEnv *env, jobject thiz, jstring path) {
    (void)thiz;
    if (!path) return -1;
    const char *cpath = (*env)->GetStringUTFChars(env, path, NULL);
    if (!cpath) return -1;
    jint rc = (jint)flipsight_load_eval(cpath);
    (*env)->ReleaseStringUTFChars(env, path, cpath);
    return rc;
}

JNIEXPORT void JNICALL
JNI_FN(nSetAnalyzeThreads)(JNIEnv *env, jobject thiz, jint n) {
    (void)env; (void)thiz;
    flipsight_set_analyze_threads((int)n);
}

JNIEXPORT void JNICALL
JNI_FN(nSetSolveThreads)(JNIEnv *env, jobject thiz, jint n) {
    (void)env; (void)thiz;
    flipsight_set_solve_threads((int)n);
}

JNIEXPORT void JNICALL
JNI_FN(nSetAnalyzeDepth)(JNIEnv *env, jobject thiz, jint d) {
    (void)env; (void)thiz;
    flipsight_set_analyze_depth((int)d);
}

JNIEXPORT jint JNICALL
JNI_FN(nAnalyzeGame)(JNIEnv *env, jobject thiz, jstring movesStr) {
    (void)thiz;
    const char *cmoves = movesStr ? (*env)->GetStringUTFChars(env, movesStr, NULL) : NULL;
    /* movesStr 가 빈 "" 이어도 GetStringUTFChars 는 "" 반환 → bridge.c 가 처리. NULL 은 -1. */
    jint count = (jint)flipsight_analyze_game(cmoves ? cmoves : "");
    if (cmoves) (*env)->ReleaseStringUTFChars(env, movesStr, cmoves);
    return count;
}

JNIEXPORT jint JNICALL
JNI_FN(nAnalyzeGameFromBoard)(JNIEnv *env, jobject thiz, jstring boardPlusTurn, jstring movesStr) {
    (void)thiz;
    const char *cboard = boardPlusTurn ? (*env)->GetStringUTFChars(env, boardPlusTurn, NULL) : NULL;
    const char *cmoves = movesStr      ? (*env)->GetStringUTFChars(env, movesStr, NULL)      : NULL;
    jint count = (jint)flipsight_analyze_game_from_board(cboard, cmoves ? cmoves : "");
    if (cmoves) (*env)->ReleaseStringUTFChars(env, movesStr, cmoves);
    if (cboard) (*env)->ReleaseStringUTFChars(env, boardPlusTurn, cboard);
    return count;
}

JNIEXPORT void JNICALL
JNI_FN(nCancelAnalyze)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    flipsight_cancel_analyze();
}

/* board_plus_turn = "<64자 보드> <X|O>", onlyMovesCsv 는 ""→NULL(필터 없음). 반환: solve_emitted(행 수). */
JNIEXPORT jint JNICALL
JNI_FN(nSearchStreamRunFiltered)(JNIEnv *env, jobject thiz,
                                 jstring boardPlusTurn, jint maxDepth, jstring onlyMovesCsv) {
    (void)thiz;
    const char *cboard = boardPlusTurn ? (*env)->GetStringUTFChars(env, boardPlusTurn, NULL) : NULL;
    const char *conly  = NULL;
    if (onlyMovesCsv) {
        conly = (*env)->GetStringUTFChars(env, onlyMovesCsv, NULL);
        if (conly && conly[0] == '\0') {     /* 빈 문자열은 필터 없음(NULL)로 정규화 */
            (*env)->ReleaseStringUTFChars(env, onlyMovesCsv, conly);
            conly = NULL;
        }
    }
    jint emitted = (jint)flipsight_search_stream_run_filtered(cboard, (int)maxDepth, conly);
    if (conly)  (*env)->ReleaseStringUTFChars(env, onlyMovesCsv, conly);
    if (cboard) (*env)->ReleaseStringUTFChars(env, boardPlusTurn, cboard);
    return emitted;
}

JNIEXPORT void JNICALL
JNI_FN(nCancelSolve)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    flipsight_cancel_solve();
}

JNIEXPORT jlong JNICALL
JNI_FN(nSolveMemBytes)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return (jlong)flipsight_solve_mem_bytes();
}
