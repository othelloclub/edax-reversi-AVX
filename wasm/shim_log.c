// wasm/shim_log.c
// root.o가 요구하는 전역 로그 심볼의 더미 정의.
// 실제 로깅은 사용하지 않으므로 최소한의 크기만 맞춰 둡니다.
typedef struct { void *f; } Log;
Log engine_log[1];
Log xboard_log[1];

