#!/usr/bin/env node
// Strata devtools relay: an MCP server over stdio that launches the viewer
// and drives it through a folder (docs/2026-11-20-devtools-design.md).
//
// The viewer side is viewer/lib/devtools.march: with STRATA_DEVTOOLS=<dir> it
// takes one-line requests from <dir>/in/<id>.req once a frame and writes
// JSON replies to <dir>/out/<id>.json. This process owns the viewer as a
// child, keeps its output for `log`, sends one request at a time, and turns
// screenshots (PPM) into PNG. No dependencies beyond Node itself.

import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import zlib from "node:zlib";
import readline from "node:readline";
import { fileURLToPath } from "node:url";

const ROOT = process.env.STRATA_ROOT || path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const VIEWER_DIR = path.join(ROOT, "viewer");
const VIEWER_BIN = path.join(VIEWER_DIR, ".march", "build", "release", "strata_viewer");
const LOG_KEEP = 5000;

// ── The viewer ────────────────────────────────────────────────────────────

let viewer = null; // { child, dir, exited, code, signal }
let logLines = []; // { n, text }
let logCount = 0;
let nextId = 1;
let queue = Promise.resolve();

function remember(text) {
  for (const line of text.split("\n")) {
    if (line === "") continue;
    logCount += 1;
    logLines.push({ n: logCount, text: line });
  }
  if (logLines.length > LOG_KEEP) logLines = logLines.slice(logLines.length - LOG_KEEP);
}

function lastLines(k) {
  return logLines.slice(-k).map((l) => l.text).join("\n");
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function running() {
  return viewer !== null && !viewer.exited;
}

function build() {
  return new Promise((resolve) => {
    const child = spawn("sh", ["-c", ". ../scripts/env.sh && forge build --release"], { cwd: VIEWER_DIR });
    let out = "";
    child.stdout.on("data", (d) => (out += d));
    child.stderr.on("data", (d) => (out += d));
    child.on("close", (code) => resolve({ ok: code === 0, out }));
  });
}

async function launch(a) {
  if (running()) throw new Error("a viewer is already running; quit it first");
  if (a.build || !fs.existsSync(VIEWER_BIN)) {
    const b = await build();
    if (!b.ok) throw new Error("the viewer did not build:\n" + b.out.split("\n").filter((l) => /error/i.test(l)).slice(0, 20).join("\n"));
  }
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "strata-devtools-"));
  const hidden = a.hidden !== false;
  const env = {
    ...process.env,
    MARCH_PIN_MAIN: "1",
    STRATA_DEVTOOLS: dir,
    STRATA_AUDIO: a.audio ? "1" : "0",
    STRATA_SIZE: a.size || "1280x800",
    STRATA_PIXELS: "1",
  };
  if (hidden) {
    env.STRATA_HIDDEN = "1";
    env.STRATA_OFFSCREEN = "1";
  }
  if (a.scene !== undefined) env.STRATA_SCENE = String(a.scene);
  if (a.radius !== undefined) env.STRATA_RADIUS = String(a.radius);
  if (a.levels !== undefined) env.STRATA_LEVELS = String(a.levels);
  if (a.scale !== undefined) env.STRATA_SCALE = String(a.scale);
  for (const [k, v] of Object.entries(a.env || {})) {
    if (!/^STRATA_[A-Z0-9_]+$/.test(k)) throw new Error("env takes STRATA_* switches only, not " + k);
    env[k] = String(v);
  }
  logLines = [];
  logCount = 0;
  const child = spawn(VIEWER_BIN, [], { cwd: VIEWER_DIR, env });
  viewer = { child, dir, exited: false, code: null, signal: null };
  const v = viewer;
  child.stdout.on("data", (d) => remember(d.toString()));
  child.stderr.on("data", (d) => remember(d.toString()));
  child.on("exit", (code, signal) => {
    v.exited = true;
    v.code = code;
    v.signal = signal;
    remember(`[relay] the viewer exited (${signal ? "signal " + signal : "code " + code})`);
  });
  // ready once it answers
  const deadline = Date.now() + (a.timeout_s || 180) * 1000;
  let st = null;
  while (Date.now() < deadline && !v.exited) {
    try {
      st = await request("status", {}, 2000);
      break;
    } catch (e) {
      if (v.exited) break;
    }
  }
  if (st === null) {
    const tail = lastLines(30);
    if (!v.exited) child.kill("SIGKILL");
    throw new Error("the viewer never answered.\n" + tail);
  }
  return { folder: dir, pid: child.pid, hidden, ...st };
}

async function quit() {
  if (!running()) return { running: false };
  const v = viewer;
  v.child.kill("SIGTERM");
  for (let i = 0; i < 50 && !v.exited; i++) await sleep(100);
  if (!v.exited) v.child.kill("SIGKILL");
  for (let i = 0; i < 20 && !v.exited; i++) await sleep(100);
  return { running: false, code: v.code, signal: v.signal };
}

function words(cmd, args) {
  const parts = [cmd];
  for (const [k, v] of Object.entries(args)) {
    if (v === undefined || v === null) continue;
    const s = String(v);
    if (!/^[A-Za-z0-9_.+-]+$/.test(s)) throw new Error(`argument ${k} must be a number or a single word`);
    parts.push(`${k}=${s}`);
  }
  return parts.join(" ");
}

// one request at a time, in order
function request(cmd, args, timeoutMs = 30000) {
  const run = async () => {
    if (!running()) throw new Error("no viewer is running; call launch first" + (viewer ? "\nlast output:\n" + lastLines(20) : ""));
    const id = String(nextId++).padStart(8, "0");
    const inDir = path.join(viewer.dir, "in");
    const outFile = path.join(viewer.dir, "out", id + ".json");
    fs.writeFileSync(path.join(inDir, id + ".tmp"), words(cmd, args) + "\n");
    fs.renameSync(path.join(inDir, id + ".tmp"), path.join(inDir, id + ".req"));
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (fs.existsSync(outFile)) {
        const reply = JSON.parse(fs.readFileSync(outFile, "utf8"));
        fs.unlinkSync(outFile);
        if (!reply.ok) throw new Error(reply.error || "the viewer refused " + cmd);
        delete reply.ok;
        delete reply.id;
        return reply;
      }
      if (!running()) throw new Error("the viewer exited while " + cmd + " waited.\n" + lastLines(30));
      await sleep(15);
    }
    // a request never taken would be taken later out of order: take it back
    try { fs.unlinkSync(path.join(inDir, id + ".req")); } catch {}
    throw new Error(`no reply to ${cmd} in ${Math.round(timeoutMs / 1000)} s`);
  };
  const p = queue.then(run, run);
  queue = p.catch(() => {});
  return p;
}

// ── PPM to PNG ─────────────────────────────────────────────────────────────

const CRC = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}

function readPpm(file) {
  const b = fs.readFileSync(file);
  // P6 <w> <h> <max> then one whitespace byte, comments allowed in the header
  let i = 0;
  const tok = () => {
    for (;;) {
      while (i < b.length && /\s/.test(String.fromCharCode(b[i]))) i++;
      if (b[i] === 0x23) { while (i < b.length && b[i] !== 0x0a) i++; continue; }
      break;
    }
    const s = i;
    while (i < b.length && !/\s/.test(String.fromCharCode(b[i]))) i++;
    return b.slice(s, i).toString("ascii");
  };
  if (tok() !== "P6") throw new Error("not a P6 PPM");
  const w = parseInt(tok(), 10), h = parseInt(tok(), 10), max = parseInt(tok(), 10);
  i += 1;
  if (max !== 255) throw new Error("PPM max value " + max);
  const px = b.slice(i);
  if (px.length < w * h * 3) return null; // still being written
  return { w, h, px };
}

function png(w, h, px) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 3 + 1)] = 0;
    px.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", zlib.deflateSync(raw, { level: 6 })),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

async function screenshot(a) {
  const r = await request("screenshot", {}, 60000);
  let img = null;
  for (let k = 0; k < 100 && img === null; k++) {
    img = readPpm(r.ppm);
    if (img === null) await sleep(20);
  }
  if (img === null) throw new Error("the screenshot was never complete: " + r.ppm);
  const data = png(img.w, img.h, img.px);
  fs.unlinkSync(r.ppm);
  let saved;
  if (a.save) {
    saved = path.resolve(a.save);
    fs.mkdirSync(path.dirname(saved), { recursive: true });
    fs.writeFileSync(saved, data);
  }
  return { data, w: img.w, h: img.h, saved };
}

// ── Tools ──────────────────────────────────────────────────────────────────

const num = (d) => ({ type: "number", description: d });
const int = (d) => ({ type: "integer", description: d });
const obj = (props, required = []) => ({ type: "object", properties: props, required, additionalProperties: false });

const TOOLS = [
  { name: "launch", description: "Start the Strata viewer (building it first if needed) with devtools on, and wait until it answers. Hidden by default (no window shown). One viewer at a time.",
    inputSchema: obj({ scene: int("a fixed camera pose to start from (STRATA_SCENE), as the golden scenes use"), size: { type: "string", description: "WxH in pixels, default 1280x800" },
      hidden: { type: "boolean", description: "false shows the window (default true)" }, radius: int("view radius in chunks (default 10)"), levels: int("far-field levels (0 none)"),
      scale: num("metres a voxel (default 1)"), audio: { type: "boolean", description: "sound on (default off)" }, build: { type: "boolean", description: "rebuild the viewer first" },
      env: { type: "object", description: "extra STRATA_* switches, as listed at the top of viewer/lib/strata_viewer.march", additionalProperties: { type: ["string", "number"] } },
      timeout_s: int("how long to wait for the first answer (default 180)") }) },
  { name: "quit", description: "Stop the running viewer.", inputSchema: obj({}) },
  { name: "log", description: "The viewer's output since a line number (each line numbered from 1 at launch); also says whether it is running.",
    inputSchema: obj({ since: int("return lines after this number (default: the last 100 lines)"), grep: { type: "string", description: "only lines containing this text" }, max: int("at most this many lines (default 200)") }) },
  { name: "status", description: "Frame number, paused or not, voxel size, and the camera.", inputSchema: obj({}) },
  { name: "camera_get", description: "The camera: position in metres, yaw and pitch in radians, flying, sun height, light mode.", inputSchema: obj({}) },
  { name: "camera_set", description: "Move and turn the camera (any subset). The view is (sin yaw cos pitch, sin pitch, -cos yaw cos pitch); y is up. Switches to flying so it stays put.",
    inputSchema: obj({ x: num("metres"), y: num("metres"), z: num("metres"), yaw: num("radians"), pitch: num("radians, -1.55..1.55") }) },
  { name: "look_at", description: "Turn the camera to face a point in metres.", inputSchema: obj({ x: num("metres"), y: num("metres"), z: num("metres") }, ["x", "y", "z"]) },
  { name: "scene", description: "Put the camera at fixed pose n, the poses the golden scenes start from.", inputSchema: obj({ n: int("scene number") }, ["n"]) },
  { name: "light", description: "Light mode (0 flat, 1 sky and sun, 2 + shadows, 3 + reflections, 4 + sky light, 5) and the sun's height in degrees.",
    inputSchema: obj({ mode: int("0..5"), sun: num("degrees above the horizon") }) },
  { name: "block_at", description: "The block id at a point in metres, its voxel, and whether its chunk is loaded.", inputSchema: obj({ x: num("metres"), y: num("metres"), z: num("metres") }, ["x", "y", "z"]) },
  { name: "raycast", description: "The first targetable block along a ray: its voxel, block id, face and distance. With no arguments, from the camera along its view.",
    inputSchema: obj({ x: num("origin, metres"), y: num("origin, metres"), z: num("origin, metres"), dx: num("direction"), dy: num("direction"), dz: num("direction"), max: num("metres, default 250") }) },
  { name: "edit", description: "Change the world through the viewer's own edit path, so fluid, physics and the save store hear of it. Coordinates are voxels. op=set (x y z block), box (x y z to x1 y1 z1, block), sphere (x y z r block), blast (x y z). block 0 is air.",
    inputSchema: obj({ op: { type: "string", enum: ["set", "box", "sphere", "blast"] }, x: int("voxel"), y: int("voxel"), z: int("voxel"), x1: int("box's far corner"), y1: int("box's far corner"), z1: int("box's far corner"), r: int("sphere radius in voxels"), block: int("block id") }, ["op", "x", "y", "z"]) },
  { name: "regenerate", description: "Reread the island's settings files and regenerate, as the R key does (1 m worlds only).", inputSchema: obj({}) },
  { name: "pause", description: "Hold the world still: walker, physics, fluid and the match stop; drawing goes on.", inputSchema: obj({}) },
  { name: "resume", description: "Let the world run again.", inputSchema: obj({}) },
  { name: "step", description: "Advance exactly n frames from a pause (pausing first if running); returns when they are done.", inputSchema: obj({ n: int("frames, default 1") }) },
  { name: "screenshot", description: "The next frame drawn, as a PNG image.", inputSchema: obj({ save: { type: "string", description: "also write the PNG to this path" } }) },
  { name: "stats", description: "The last frame's numbers: frame, frame_ms, march_ms, shim_ms, waiting_ms, gpu_ms, draw_calls, fluid_ms, tick_ms, fluid_cells, paused.", inputSchema: obj({}) },
];

const FORWARDED = new Set(["status", "camera_get", "camera_set", "look_at", "scene", "light", "block_at", "raycast", "edit", "regenerate", "pause", "resume", "stats"]);

const text = (v) => ({ content: [{ type: "text", text: typeof v === "string" ? v : JSON.stringify(v, null, 2) }] });

async function call(name, a) {
  if (name === "launch") return text(await launch(a));
  if (name === "quit") return text(await quit());
  if (name === "log") {
    const max = a.max || 200;
    let ls = a.since !== undefined ? logLines.filter((l) => l.n > a.since) : logLines.slice(-100);
    if (a.grep) ls = ls.filter((l) => l.text.includes(a.grep));
    ls = ls.slice(-max);
    return text({ running: running(), last: logCount, lines: ls.map((l) => `${l.n}: ${l.text}`) });
  }
  if (name === "step") return text(await request("step", { n: a.n ?? 1 }, 120000));
  if (name === "screenshot") {
    const s = await screenshot(a);
    const content = [{ type: "image", data: s.data.toString("base64"), mimeType: "image/png" }];
    content.push({ type: "text", text: `${s.w} x ${s.h}` + (s.saved ? `, saved to ${s.saved}` : "") });
    return { content };
  }
  if (FORWARDED.has(name)) return text(await request(name, a));
  throw new Error("no tool " + name);
}

// ── MCP over stdio (newline-delimited JSON-RPC 2.0) ────────────────────────

function send(msg) {
  process.stdout.write(JSON.stringify(msg) + "\n");
}

async function handle(msg) {
  const { id, method, params } = msg;
  const reply = (result) => id !== undefined && send({ jsonrpc: "2.0", id, result });
  const error = (code, message) => id !== undefined && send({ jsonrpc: "2.0", id, error: { code, message } });
  if (method === "initialize") {
    return reply({ protocolVersion: params?.protocolVersion || "2025-06-18", capabilities: { tools: {} },
      serverInfo: { name: "strata-devtools", version: "0.1.0" },
      instructions: "Drive a running Strata viewer: launch it, look (screenshot, camera), ask (block_at, raycast, stats), change (edit, light, regenerate), hold time (pause, step), read its output (log), quit. Positions are metres except edit, which is voxels." });
  }
  if (method === "notifications/initialized" || method?.startsWith("notifications/")) return;
  if (method === "ping") return reply({});
  if (method === "tools/list") return reply({ tools: TOOLS });
  if (method === "tools/call") {
    try {
      return reply(await call(params.name, params.arguments || {}));
    } catch (e) {
      return reply({ content: [{ type: "text", text: String(e.message || e) }], isError: true });
    }
  }
  return error(-32601, "no method " + method);
}

const rl = readline.createInterface({ input: process.stdin });
rl.on("line", (line) => {
  if (line.trim() === "") return;
  let msg;
  try { msg = JSON.parse(line); } catch { return send({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "parse error" } }); }
  handle(msg);
});
rl.on("close", async () => {
  await quit();
  process.exit(0);
});
for (const sig of ["SIGINT", "SIGTERM"]) process.on(sig, async () => { await quit(); process.exit(0); });
