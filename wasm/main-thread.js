export class FlipsightWasmClient {
  constructor({ workerUrl, loaderUrl, exportName = 'FlipsightModule' }) {
    this.worker = new Worker(workerUrl, { type: 'module' });
    this.ready = new Promise((resolve, reject) => {
      const onMsg = (e) => {
        const { type } = e.data || {};
        if (type === 'LOADED') {
          this.worker.removeEventListener('message', onMsg);
          resolve();
        } else if (type === 'ERROR') {
          this.worker.removeEventListener('message', onMsg);
          reject(new Error(e.data.error));
        }
      };
      this.worker.addEventListener('message', onMsg);
    });
    this.worker.postMessage({ type: 'LOAD', loaderUrl, exportName });
  }

  async analyze({ board, turn = 'B', hint_n = 1 }) {
    await this.ready;
    return new Promise((resolve, reject) => {
      const onMsg = (e) => {
        const { type, payload, error } = e.data || {};
        if (type === 'RESULT') {
          this.worker.removeEventListener('message', onMsg);
          resolve(payload);
        } else if (type === 'ERROR') {
          this.worker.removeEventListener('message', onMsg);
          reject(new Error(error));
        }
      };
      this.worker.addEventListener('message', onMsg);
      this.worker.postMessage({ type: 'ANALYZE', payload: { board, turn, hint_n }});
    });
  }

  terminate() {
    this.worker?.terminate();
  }
}
