const keys = new Map([
  ['KeyW', 1], ['KeyA', 2], ['KeyS', 4], ['KeyD', 8],
  ['Space', 16], ['ShiftLeft', 32],
]);

export function createInput(canvas, onLockChange) {
  const controller = new AbortController();
  const options = { signal: controller.signal };
  let held = 0, pressed = 0, dx = 0, dy = 0;
  let active = false;

  function clear() {
    held = pressed = dx = dy = 0;
  }

  document.addEventListener('pointerlockchange', () => {
    active = document.pointerLockElement === canvas;
    clear();
    onLockChange(active);
  }, options);
  window.addEventListener('blur', clear, options);
  document.addEventListener('visibilitychange', clear, options);
  window.addEventListener('keydown', (event) => {
    const bit = keys.get(event.code);
    if (active && bit) {
      held |= bit;
      event.preventDefault();
    }
  }, options);
  window.addEventListener('keyup', (event) => { held &= ~(keys.get(event.code) ?? 0); }, options);
  document.addEventListener('mousemove', (event) => {
    if (active) {
      dx -= event.movementX;
      dy += event.movementY;
    }
  }, options);
  canvas.addEventListener('mousedown', (event) => {
    if (!active) return;
    if (event.button === 0) pressed |= 64;
    if (event.button === 2) pressed |= 128;
  }, options);
  canvas.addEventListener('contextmenu', (event) => event.preventDefault(), options);

  return {
    clear,
    sample() {
      const state = { held, pressed, dx, dy };
      pressed = dx = dy = 0;
      return state;
    },
    dispose() { controller.abort(); },
  };
}
