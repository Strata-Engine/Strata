#!/usr/bin/env node
// End-to-end check of the devtools relay: speaks MCP to server.mjs over
// stdio, launches a hidden viewer, drives it through every tool and checks
// the answers. Needs a GPU. Run from anywhere: node tools/strata-mcp/check.mjs

import { spawn } from "node:child_process";
import path from "node:path";
import readline from "node:readline";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const server = spawn(process.execPath, [path.join(here, "server.mjs")], { stdio: ["pipe", "pipe", "inherit"] });
const pending = new Map();
let nextId = 1;
readline.createInterface({ input: server.stdout }).on("line", (line) => {
  const m = JSON.parse(line);
  const p = pending.get(m.id);
  if (p) { pending.delete(m.id); p(m); }
});
function rpc(method, params) {
  const id = nextId++;
  server.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
  return new Promise((r) => pending.set(id, r));
}
async function tool(name, args = {}) {
  const m = await rpc("tools/call", { name, arguments: args });
  if (m.error) throw new Error(`${name}: ${m.error.message}`);
  const r = m.result;
  if (r.isError) throw new Error(`${name}: ${r.content[0].text}`);
  return r;
}
const json = (r) => JSON.parse(r.content[0].text);
let failures = 0;
function check(what, ok, detail = "") {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail ? ": " + detail : ""}`);
  if (!ok) failures++;
}

try {
  const init = await rpc("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "check", version: "0" } });
  check("initialize", init.result?.serverInfo?.name === "strata-devtools");
  const list = await rpc("tools/list", {});
  check("tools/list", list.result.tools.length >= 18, `${list.result.tools.length} tools`);

  const l = json(await tool("launch", { size: "640x400", radius: 4, levels: 0, scene: 1 }));
  check("launch answers", typeof l.frame === "number" && l.voxel === 1, `frame ${l.frame}`);

  const c = json(await tool("camera_set", { x: 10, y: 90, z: 10, yaw: 0, pitch: -0.5 }));
  check("camera_set", c.x === 10 && c.y === 90 && c.flying === true);
  const g = json(await tool("camera_get"));
  check("camera_get keeps the place", Math.abs(g.x - 10) < 0.01 && Math.abs(g.y - 90) < 0.01, JSON.stringify(g));

  const hit = json(await tool("raycast"));
  check("raycast from the camera hits the ground", hit.hit === true && hit.block > 0, JSON.stringify(hit));

  const b = json(await tool("block_at", { x: hit.vx + 0.5, y: hit.vy + 0.5, z: hit.vz + 0.5 }));
  check("block_at agrees with the ray", b.block === hit.block && b.loaded === true);

  await tool("edit", { op: "set", x: hit.vx, y: hit.vy, z: hit.vz, block: 0 });
  await tool("step", { n: 2 });
  const b2 = json(await tool("block_at", { x: hit.vx + 0.5, y: hit.vy + 0.5, z: hit.vz + 0.5 }));
  check("edit set the block to air", b2.block === 0, JSON.stringify(b2));

  const p0 = json(await tool("status"));
  check("step leaves it paused", p0.paused === true);
  await new Promise((r) => setTimeout(r, 300));
  const p1 = json(await tool("stats"));
  check("stats while paused", p1.paused === true && typeof p1.gpu_ms === "number", JSON.stringify(p1));

  const lt = json(await tool("light", { mode: 2, sun: 20 }));
  check("light", lt.light === 2 && lt.sun === 20);

  const la = json(await tool("look_at", { x: 10, y: 0, z: 10 }));
  check("look_at straight down", la.pitch < -1.5, `pitch ${la.pitch}`);

  const shot = await tool("screenshot");
  const img = shot.content.find((x) => x.type === "image");
  const head = img ? Buffer.from(img.data, "base64").subarray(0, 8).toString("hex") : "";
  check("screenshot is a PNG", head === "89504e470d0a1a0a", shot.content.find((x) => x.type === "text")?.text);

  await tool("resume");
  const r = json(await tool("status"));
  check("resume", r.paused === false);

  let refused = false;
  try { await tool("edit", { op: "melt", x: 0, y: 0, z: 0 }); } catch { refused = true; }
  check("a bad edit is refused", refused);

  const lg = json(await tool("log", { grep: "devtools" }));
  check("log carries the viewer's output", lg.lines.some((s) => s.includes("taking requests")));

  const q = json(await tool("quit"));
  check("quit", q.running === false);
} catch (e) {
  check("no tool failed", false, String(e.message || e));
}
server.stdin.end();
console.log(failures === 0 ? "devtools: PASS" : `devtools: ${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
