// Match the desktop body's local-to-world quaternion transform to find the
// parcel cells its shadow volume overlaps.
function deckBounds(deck, poses) {
  const [px, py, pz, scale, qx, qy, qz, qw] = poses.subarray(deck[0] * 8, deck[0] * 8 + 8);
  const low = [Infinity, Infinity, Infinity];
  const high = [-Infinity, -Infinity, -Infinity];
  for (const x of [0, scale * deck[1]]) {
    for (const y of [0, scale * deck[2]]) {
      for (const z of [0, scale * deck[3]]) {
        const tx = 2 * (qy * z - qz * y);
        const ty = 2 * (qz * x - qx * z);
        const tz = 2 * (qx * y - qy * x);
        const position = [px + x + qw * tx + qy * tz - qz * ty,
          py + y + qw * ty + qz * tx - qx * tz,
          pz + z + qw * tz + qx * ty - qy * tx];
        for (let axis = 0; axis < 3; axis++) {
          const chunk = Math.floor(position[axis] / 8);
          low[axis] = Math.min(low[axis], chunk);
          high[axis] = Math.max(high[axis], chunk);
        }
      }
    }
  }
  return [low, high];
}

export function buildDeckGrid(decks, poses, width, depth) {
  const cells = Array.from({ length: width * depth * 4 }, () => []);
  let count = 0;
  for (let i = 0; i < decks.length; i++) {
    const [[minX, minY, minZ], [maxX, maxY, maxZ]] = deckBounds(decks[i], poses);
    for (let z = Math.max(0, minZ); z <= Math.min(depth - 1, maxZ); z++) {
      for (let x = Math.max(0, minX); x <= Math.min(width - 1, maxX); x++) {
        for (let y = Math.max(0, minY); y <= Math.min(3, maxY); y++) {
          cells[(x + z * width) * 4 + y].push(i);
          count++;
        }
      }
    }
  }
  const tableWords = cells.length * 2;
  const bodies = tableWords + count;
  const stride = decks[0]?.length ?? 0;
  const grid = new Uint32Array(bodies + decks.length * stride);
  let offset = tableWords;
  for (let i = 0; i < cells.length; i++) {
    grid[i * 2] = offset;
    grid[i * 2 + 1] = cells[i].length;
    for (const deck of cells[i]) grid[offset++] = bodies + deck * stride;
  }
  for (let i = 0; i < decks.length; i++) grid.set(decks[i], bodies + i * stride);
  return grid;
}
