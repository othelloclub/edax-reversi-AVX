#include <emscripten/emscripten.h>
int main(void) {
  // JS에서 cwrap 함수들을 쓰기 때문에 여기선 아무것도 안 해도 됨
  emscripten_exit_with_live_runtime(); // 런타임이 종료되지 않도록 유지
  return 0;
}