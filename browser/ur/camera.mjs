// The camera and clip planes from Ur.Scene.camera, in metres.
const eyeOffset = 194;
const eyeY = 151;
const aimY = 3;
const near = 0.1;
const far = 800;
const slope = eyeY - aimY;
const length = Math.hypot(eyeOffset, slope);
const defaultAspect = 900 / 660;

export const regionCamera = Object.freeze({ cx: 234, cz: 176, zoom: 345, yaw: 0.55 });
export const trafficCamera = Object.freeze({ cx: 94, cz: 147, zoom: 38, yaw: 0.55 });

export function cameraProjection({ cx, cz, zoom, yaw }, aspect = defaultAspect) {
  const sin = Math.sin(yaw), cos = Math.cos(yaw);
  const sx = 1 / zoom, sy = aspect / zoom, sz = 1 / (far - near);
  const eye = [cx + eyeOffset * sin, eyeY, cz + eyeOffset * cos];
  const right = [cos * sx, 0, -sin * sx];
  const up = [-slope * sin / length * sy, eyeOffset / length * sy, -slope * cos / length * sy];
  const forward = [-eyeOffset * sin / length * sz, -slope / length * sz, -eyeOffset * cos / length * sz];
  const translation = row => -row.reduce((sum, component, axis) => sum + component * eye[axis], 0);
  return [...eye, 0, ...right, translation(right), ...up, translation(up),
    ...forward, translation(forward) - near * sz];
}

// Ground intercept and X/Z displacement per unit of height, in parcels.
export function parcelRay({ cx, cz, zoom, yaw }, u, v, aspect = defaultAspect) {
  const s = Math.sin(yaw), c = Math.cos(yaw);
  const horizontal = (2 * u - 1) * zoom;
  const vertical = (1 - 2 * v) * zoom / aspect;
  const distance = (aimY * eyeOffset + vertical * length) / slope;
  return [(cx + horizontal * c - distance * s) / 8, (cz - horizontal * s - distance * c) / 8,
    eyeOffset * s / slope, eyeOffset * c / slope];
}
