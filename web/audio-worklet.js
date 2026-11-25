// Plays the game's mixer output from the ring buffer in the WebAssembly memory (runtime/audio_web.c).
// Header (int32): [0] frames written, [1] frames read (here), [2] output running, [3] underruns.
class PoptbAudio extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { buffer, ptr, n } = options.processorOptions;
    this.hdr = new Int32Array(buffer, ptr, 4);
    this.data = new Float32Array(buffer, ptr + 16, n * 2);
    this.mask = n - 1;
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0];
    const w = Atomics.load(this.hdr, 0);
    let r = Atomics.load(this.hdr, 1);
    let missing = 0;
    for (let i = 0; i < L.length; i++) {
      if (r !== w) {
        const j = (r & this.mask) * 2;
        L[i] = this.data[j]; R[i] = this.data[j + 1];
        r = (r + 1) | 0;
      } else { L[i] = R[i] = 0; missing++; }
    }
    Atomics.store(this.hdr, 1, r);
    if (missing) Atomics.add(this.hdr, 3, 1);
    return true;
  }
}
registerProcessor('poptb-audio', PoptbAudio);
