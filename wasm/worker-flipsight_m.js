// wasm/worker-flipsight_m.js
import ModuleFactory from './flipsight_m.js';

let Module;
let flipsight_init, flipsight_setboard, flipsight_hint;

async function ensureReady() {
  if (Module) return;
  Module = await ModuleFactory({
    locateFile: (p) => {
      if (p.endsWith('.wasm')) return './flipsight_m.wasm';
      if (p.endsWith('.data')) return './flipsight.data';
      return p;
    },
    print: (x) => self.postMessage({ type: 'log', msg: String(x) }),
    printErr: (x) => self.postMessage({ type: 'log', msg: '[err] ' + String(x) }),
    onAbort: (r) => self.postMessage({ type: 'log', msg: '[abort] ' + String(r) }),
  });

  const cwrapCompat = (names, returnType, argTypes) => {
    for (const name of names) {
      try { return Module.cwrap(name, returnType, argTypes); } catch (_) {}
    }
    return null;
  };
  flipsight_init = cwrapCompat(['flipsight_init', 'edax_init'], 'number', []);
  flipsight_setboard = cwrapCompat(['flipsight_setboard', 'edax_setboard'], 'number', ['string']);
  flipsight_hint = cwrapCompat(['flipsight_hint', 'edax_hint'], 'string', []);

  // (선택) 사전 초기화
  flipsight_init();

  // (선택) book.dat 존재 체크 로그
  try {
    const ok = Module.FS.analyzePath('/data/book.dat').exists;
    self.postMessage({ type: 'log', msg: `book.dat in FS: ${ok ? 'YES' : 'NO'}` });
  } catch (e) {
    self.postMessage({ type: 'log', msg: `[fs-check-error] ${e}` });
  }

  self.postMessage({ type: 'ready' });
}

self.onmessage = async (e) => {
  const { type, payload } = e.data;
  await ensureReady();

  try {
    if (type === 'setboard') {
      const { board64, turn } = payload; // turn: 'B' | 'W'
      // bridge.c는 "64글자 + 공백 + X/O" 포맷 기대
      const toMove = (turn === 'B') ? 'X' : 'O';
      const nboard = `${board64} ${toMove}`;
      const rc = flipsight_setboard(nboard);
      self.postMessage({ type: 'setboard:done', rc });
    }
    else if (type === 'hint') {
      const best = flipsight_hint(); // C 문자열 바로 반환
      self.postMessage({ type: 'hint:done', rc: 0, bestMove: best || null });
    }
  } catch (err) {
    self.postMessage({ type: 'error', message: String(err && err.message ? err.message : err) });
  }
};
