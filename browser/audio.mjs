const EDIT = 1;

export function createAudio() {
  let context;
  let disposed = false;

  async function resume() {
    if (disposed) return;
    try {
      const Audio = globalThis.AudioContext;
      if (!Audio) return;
      context ??= new Audio();
      if (context.state !== 'running') await context.resume();
    } catch {
      // Audio may be unavailable or blocked. The next play gesture can retry.
    }
  }

  function playEvents(bits) {
    if (!(bits & EDIT) || context?.state !== 'running' || disposed) return;

    const start = context.currentTime;
    const end = start + 0.075;
    const tone = context.createOscillator();
    const gain = context.createGain();
    tone.type = 'triangle';
    tone.frequency.setValueAtTime(240, start);
    tone.frequency.exponentialRampToValueAtTime(75, end);
    gain.gain.setValueAtTime(0, start);
    gain.gain.linearRampToValueAtTime(0.15, start + 0.004);
    gain.gain.exponentialRampToValueAtTime(0.001, end);
    tone.connect(gain);
    gain.connect(context.destination);
    tone.onended = () => {
      tone.disconnect();
      gain.disconnect();
    };
    tone.start(start);
    tone.stop(end);
  }

  function dispose() {
    if (disposed) return;
    disposed = true;
    if (context && context.state !== 'closed') void context.close().catch(() => {});
  }

  return { resume, playEvents, dispose };
}
