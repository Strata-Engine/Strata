#!/usr/bin/env node
// Strata devtools relay: an MCP server over stdio that launches a Strata app (the viewer by
// default, or a game such as Ur) and drives it through a folder
// (docs/2026-11-20-devtools-design.md).
//
// The app side is lib/strata/devtools.march (Strata.Devtools): with STRATA_DEVTOOLS=<dir> it
// takes one-line requests from <dir>/in/<id>.req once a frame and writes JSON replies to
// <dir>/out/<id>.json. This process owns the app as a child, keeps its output for `log`,
// sends one request at a time, and turns screenshots (PPM) into PNG. It also runs the
// analyses that need the process from outside: bench_compare (alternating runs of two
// builds), profile (macOS `sample`, runtime time charged to the March function that caused
// it) and screenshot_diff. No dependencies beyond Node itself.
//
// A project can make its own app the default: STRATA_MCP_APP_BIN and STRATA_MCP_APP_CWD in
// the server's environment (Ur's .mcp.json sets them).

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
const DEFAULT_APP = process.env.STRATA_MCP_APP_BIN
  ? { bin: path.resolve(process.env.STRATA_MCP_APP_BIN), cwd: path.resolve(process.env.STRATA_MCP_APP_CWD || path.dirname(process.env.STRATA_MCP_APP_BIN)) }
  : null;
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

// The app to launch: a.app ({bin, cwd, env}), the project's default, or the viewer.
function appOf(a) {
  if (a.app && a.app.bin) return { bin: path.resolve(a.app.bin), cwd: path.resolve(a.app.cwd || path.dirname(a.app.bin)), env: a.app.env || {}, viewer: false };
  if (DEFAULT_APP && !a.viewer) return { ...DEFAULT_APP, env: {}, viewer: false };
  return { bin: VIEWER_BIN, cwd: VIEWER_DIR, env: {}, viewer: true };
}

async function launch(a) {
  if (running()) throw new Error("an app is already running; quit it first");
  const app = appOf(a);
  if (app.viewer && (a.build || !fs.existsSync(VIEWER_BIN))) {
    const b = await build();
    if (!b.ok) throw new Error("the viewer did not build:\n" + b.out.split("\n").filter((l) => /error/i.test(l)).slice(0, 20).join("\n"));
  }
  if (!fs.existsSync(app.bin)) throw new Error("no app at " + app.bin + " (build it first)");
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
  for (const [k, v] of Object.entries({ ...app.env, ...(a.env || {}) })) {
    if (!/^(STRATA|MARCH|UR)_[A-Z0-9_]+$/.test(k)) throw new Error("env takes STRATA_*, MARCH_* or UR_* switches only, not " + k);
    env[k] = String(v);
  }
  logLines = [];
  logCount = 0;
  const child = spawn(app.bin, [], { cwd: app.cwd, env });
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
  return { app: app.bin, folder: dir, pid: child.pid, hidden, ...st };
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


// ── Analyses run from outside the app ────────────────────────────────────────

function summary(xs) {
  if (xs.length === 0) return { n: 0, median: 0, p90: 0, slow10: 0, max: 0 };
  const s = [...xs].sort((x, y) => x - y);
  const n = s.length;
  const tail = s.slice(n - Math.max(1, Math.floor(n / 10)));
  const r = (v) => Math.round(v * 1000) / 1000;
  return { n, median: r(s[Math.floor(n / 2)]), p90: r(s[Math.floor((n * 9) / 10)]), slow10: r(tail.reduce((a, b) => a + b, 0) / tail.length), max: r(s[n - 1]) };
}

// One run of one build: launch hidden, let it draw `frames` frames, then read each field's
// values over the window.
async function benchRun(build, a, fields) {
  const frames = a.frames || 160;
  await launch({ app: build, env: a.env, hidden: true, timeout_s: 300 });
  try {
    for (;;) {
      const st = await request("status", {}, 30000);
      if ((st.recorded ?? st.frame ?? 0) >= frames) break;
      if (!running()) throw new Error("the app exited during a run:\n" + lastLines(20));
      await sleep(100);
    }
    // hold the world so the recording stops growing while it is read
    await request("pause", {}, 30000);
    const win = a.from_s !== undefined || a.to_s !== undefined
      ? { frames: 100000, from: a.from_s, to: a.to_s }
      : { frames: 100000 };
    const out = {};
    for (const f of fields) {
      const r = await request("perf_values", { field: f, ...win }, 30000);
      let vs = r.values;
      if (a.from_s === undefined && a.to_s === undefined) {
        // frame window: values are oldest first, frame 1 first
        const lo = (a.window_from ?? 10) - 1, hi = a.window_to ?? frames;
        vs = vs.slice(lo, hi);
      }
      out[f] = vs;
    }
    return out;
  } finally {
    await quit();
  }
}

async function benchCompare(a) {
  if (running()) await quit();
  const fields = a.fields || ["frame_ms", "sim_ms", "renderer_ms", "gpu_ms"];
  const rounds = a.rounds || 6;
  const pooled = { a: {}, b: {} }, perRun = { a: [], b: [] };
  for (const f of fields) { pooled.a[f] = []; pooled.b[f] = []; }
  for (let r = 0; r < rounds; r++) {
    const order = r % 2 === 0 ? ["a", "b"] : ["b", "a"];
    for (const k of order) {
      const vals = await benchRun(a[k], a, fields);
      for (const f of fields) pooled[k][f].push(...vals[f]);
      perRun[k].push(summary(vals[fields[0]]).median);
    }
  }
  const result = { rounds, window: a.from_s !== undefined || a.to_s !== undefined ? `game ${a.from_s ?? "-"}..${a.to_s ?? "-"} s` : `frames ${a.window_from ?? 10}..${a.window_to ?? (a.frames || 160)}`, a: {}, b: {}, delta_median_pct: {} };
  for (const f of fields) {
    result.a[f] = summary(pooled.a[f]);
    result.b[f] = summary(pooled.b[f]);
    const ma = result.a[f].median, mb = result.b[f].median;
    result.delta_median_pct[f] = ma ? Math.round(((mb - ma) / ma) * 1000) / 10 : null;
  }
  result.per_run_medians = { a: perRun.a.map((v) => Math.round(v * 100) / 100), b: perRun.b.map((v) => Math.round(v * 100) / 100) };
  return result;
}

// `sample` output: one call tree per thread, each line "<indent><count> <name>  (in <image>)".
function sampleThreads(file) {
  const lines = fs.readFileSync(file, "utf8").split("\n");
  const threads = [];
  let cur = null;
  for (const l of lines) {
    if (/^\s{4}\d+ Thread_/.test(l)) { cur = [l]; threads.push(cur); }
    else if (l.startsWith("Total number")) cur = null;
    else if (cur) cur.push(l);
  }
  if (threads.length === 0) throw new Error("no threads in the sample");
  return threads;
}

// Samples spent waiting (sleep, condition variables, kernel waits) are not work.
const IDLE = /^(nanosleep|__semwait_signal|__psynch_cvwait|__psynch_mutexwait|mach_msg2?_trap|__workq_kernreturn|kevent|kevent64|__select|poll|__ulock_wait2?|_pthread_cond_wait|semaphore_wait_trap)\b/;
const RUNTIME = ["march_", "mi_", "_platform_memmove", "native_", "__drop", "__march_rc", "go$", "rev_onto", "$lam", "proc_trampoline", "_ctx_start", "_sigtramp"];

// One thread's inclusive counts per function (excluding idle leaves' samples from every
// ancestor), and for each function the non-runtime caller that owns it.
function threadCounts(t) {
  const re = /^([\s+!:|]*)(\d+)\s+(.+?)\s+\(in /;
  const rows = [];
  for (const l of t.slice(1)) {
    const m = re.exec(l);
    if (m) rows.push({ d: m[1].length, c: parseInt(m[2], 10), name: m[3] });
  }
  // idle samples below each row, so they can be taken off its inclusive count
  const idleBelow = new Array(rows.length).fill(0);
  for (let i = 0; i < rows.length; i++) {
    if (!IDLE.test(rows[i].name)) continue;
    // charge to this row and every ancestor
    let d = rows[i].d;
    idleBelow[i] += rows[i].c;
    for (let j = i - 1; j >= 0 && d > 0; j--) if (rows[j].d < d) { idleBelow[j] += rows[i].c; d = rows[j].d; }
  }
  const incl = new Map(), callers = new Map(), stack = [];
  let busy = 0;
  for (let i = 0; i < rows.length; i++) {
    const { d, name } = rows[i], c = rows[i].c - idleBelow[i];
    while (stack.length && stack[stack.length - 1][0] >= d) stack.pop();
    if (stack.length === 0) busy += Math.max(0, c);
    if (c > 0 && !stack.some(([, n]) => n === name)) {
      incl.set(name, (incl.get(name) || 0) + c);
      const owner = [...stack].reverse().find(([, n]) => !RUNTIME.some((p) => n.startsWith(p)));
      const key = owner ? owner[1] : "?";
      if (!callers.has(name)) callers.set(name, new Map());
      callers.get(name).set(key, (callers.get(name).get(key) || 0) + c);
    }
    stack.push([d, name]);
  }
  return { busy, incl, callers };
}

function profileFile(file, a) {
  const threads = sampleThreads(file).map((t) => ({ name: t[0].trim().replace(/^\d+\s+/, "").slice(0, 70), ...threadCounts(t) }));
  const chosen = a.thread === "busiest" ? [threads.reduce((x, y) => (y.busy > x.busy ? y : x))] : threads;
  const total = chosen.reduce((s, t) => s + t.busy, 0) || 1;
  const incl = new Map(), callers = new Map();
  for (const t of chosen) {
    for (const [n, v] of t.incl) incl.set(n, (incl.get(n) || 0) + v);
    for (const [n, m] of t.callers) {
      if (!callers.has(n)) callers.set(n, new Map());
      for (const [k, v] of m) callers.get(n).set(k, (callers.get(n).get(k) || 0) + v);
    }
  }
  const pct = (v) => Math.round((1000 * v) / total) / 10;
  const top = [...incl.entries()].sort((x, y) => y[1] - x[1]).slice(0, a.top || 30).map(([n, v]) => ({ pct: pct(v), fn: n.slice(0, 140) }));
  const focus = a.focus || ["march_alloc", "_platform_memmove", "native_int_arr_set", "native_i32_arr_set", "native_f32_arr_set", "mi_malloc", "mi_free", "__drop", "march_rc", "march_alloc_float"];
  const charged = [];
  for (const [n, v] of incl.entries()) {
    if (!focus.some((p) => n.startsWith(p)) || pct(v) < 0.5) continue;
    const by = [...callers.get(n).entries()].sort((x, y) => y[1] - x[1]).slice(0, 4).map(([k, w]) => ({ fn: k.slice(0, 100), pct: pct(w) }));
    charged.push({ fn: n.slice(0, 100), pct: pct(v), charged_to: by });
  }
  charged.sort((x, y) => y.pct - x.pct);
  return {
    busy_samples: total,
    threads: threads.filter((t) => t.busy > 0).sort((x, y) => y.busy - x.busy).slice(0, 8).map((t) => ({ thread: t.name, busy_pct: Math.round((1000 * t.busy) / total) / 10 })),
    top, runtime_charged_to_callers: charged.slice(0, 15),
  };
}

async function profile(a) {
  if (!running()) throw new Error("no app is running; call launch first");
  const secs = a.seconds || 10;
  const file = path.join(viewer.dir, `sample-${Date.now()}.txt`);
  await new Promise((resolve, reject) => {
    const child = spawn("sample", [String(viewer.child.pid), String(secs), "-file", file], { stdio: "ignore" });
    child.on("error", reject);
    child.on("close", () => resolve());
  });
  if (!fs.existsSync(file)) throw new Error("sample wrote nothing (macOS only; the app may have exited)");
  return { file, ...profileFile(file, a) };
}

// PNG (8-bit RGB or RGBA, not interlaced) or PPM to {w, h, px: RGB bytes}.
function readImage(file) {
  if (/\.ppm$/i.test(file)) {
    const img = readPpm(file);
    if (!img) throw new Error("incomplete PPM " + file);
    return img;
  }
  const b = fs.readFileSync(file);
  if (b.readUInt32BE(0) !== 0x89504e47) throw new Error("not a PNG or PPM: " + file);
  let o = 8, w = 0, h = 0, ct = 0, depth = 0, interlace = 0;
  const idat = [];
  while (o < b.length) {
    const len = b.readUInt32BE(o), type = b.toString("ascii", o + 4, o + 8), data = b.subarray(o + 8, o + 8 + len);
    if (type === "IHDR") { w = data.readUInt32BE(0); h = data.readUInt32BE(4); depth = data[8]; ct = data[9]; interlace = data[12]; }
    else if (type === "IDAT") idat.push(data);
    else if (type === "IEND") break;
    o += 12 + len;
  }
  if (depth !== 8 || (ct !== 2 && ct !== 6) || interlace !== 0) throw new Error(`unsupported PNG (depth ${depth}, colour type ${ct}, interlace ${interlace}): ${file}`);
  const bpp = ct === 6 ? 4 : 3, stride = w * bpp;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const out = Buffer.alloc(w * h * 3), line = Buffer.alloc(stride), prev = Buffer.alloc(stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let i = 0; i < stride; i++) {
      const left = i >= bpp ? line[i - bpp] : 0, up = prev[i], ul = i >= bpp ? prev[i - bpp] : 0;
      let v = src[i];
      if (f === 1) v += left;
      else if (f === 2) v += up;
      else if (f === 3) v += (left + up) >> 1;
      else if (f === 4) { const p = left + up - ul, pa = Math.abs(p - left), pb = Math.abs(p - up), pc = Math.abs(p - ul); v += pa <= pb && pa <= pc ? left : pb <= pc ? up : ul; }
      line[i] = v & 255;
    }
    for (let x = 0; x < w; x++) { out[(y * w + x) * 3] = line[x * bpp]; out[(y * w + x) * 3 + 1] = line[x * bpp + 1]; out[(y * w + x) * 3 + 2] = line[x * bpp + 2]; }
    line.copy(prev);
  }
  return { w, h, px: out };
}

async function screenshotDiff(a) {
  let bPath = a.b;
  if (a.take) {
    const s = await screenshot({});
    bPath = path.join(viewer.dir, `diff-b-${Date.now()}.png`);
    fs.writeFileSync(bPath, s.data);
  }
  if (!bPath) throw new Error("give b, or take=true");
  const A = readImage(path.resolve(a.a)), B = readImage(path.resolve(bPath));
  if (A.w !== B.w || A.h !== B.h) return { same_size: false, a: `${A.w}x${A.h}`, b: `${B.w}x${B.h}` };
  const thr = a.threshold || 0;
  let n = 0, maxd = 0, x0 = A.w, y0 = A.h, x1 = -1, y1 = -1;
  const diff = a.save_diff ? Buffer.alloc(A.w * A.h * 3) : null;
  for (let y = 0; y < A.h; y++) for (let x = 0; x < A.w; x++) {
    const i = (y * A.w + x) * 3;
    const d = Math.max(Math.abs(A.px[i] - B.px[i]), Math.abs(A.px[i + 1] - B.px[i + 1]), Math.abs(A.px[i + 2] - B.px[i + 2]));
    if (d > maxd) maxd = d;
    const changed = d > thr;
    if (changed) { n++; if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y; }
    if (diff) {
      if (changed) { diff[i] = 255; diff[i + 1] = 0; diff[i + 2] = 0; }
      else { diff[i] = A.px[i] >> 2; diff[i + 1] = A.px[i + 1] >> 2; diff[i + 2] = A.px[i + 2] >> 2; }
    }
  }
  let saved;
  if (diff) { saved = path.resolve(a.save_diff); fs.mkdirSync(path.dirname(saved), { recursive: true }); fs.writeFileSync(saved, png(A.w, A.h, diff)); }
  return { same_size: true, identical: n === 0, pixels_differing: n, of: A.w * A.h, largest_difference: maxd, bbox: n ? { x0, y0, x1, y1 } : null, b: bPath, diff_image: saved };
}

// ── Tools ──────────────────────────────────────────────────────────────────

const num = (d) => ({ type: "number", description: d });
const int = (d) => ({ type: "integer", description: d });
const obj = (props, required = []) => ({ type: "object", properties: props, required, additionalProperties: false });

const TOOLS = [
  { name: "launch", description: "Start a Strata app with devtools on and wait until it answers: the project's default app (STRATA_MCP_APP_BIN), an app given as {bin, cwd, env}, or the viewer (building it first if needed). Hidden by default (no window shown). One app at a time.",
    inputSchema: obj({ scene: int("a fixed camera pose to start from (STRATA_SCENE), as the golden scenes use"), size: { type: "string", description: "WxH in pixels, default 1280x800" },
      hidden: { type: "boolean", description: "false shows the window (default true)" }, radius: int("view radius in chunks (default 10)"), levels: int("far-field levels (0 none)"),
      scale: num("metres a voxel (default 1)"), audio: { type: "boolean", description: "sound on (default off)" }, build: { type: "boolean", description: "rebuild the viewer first" },
      env: { type: "object", description: "extra STRATA_*, UR_* or MARCH_* switches (the viewer's are listed at the top of viewer/lib/strata_viewer.march)", additionalProperties: { type: ["string", "number"] } },
      app: { type: "object", description: "the app to launch instead: {bin: its binary, cwd: where it runs (default the binary's folder), env: switches}" },
      viewer: { type: "boolean", description: "launch the viewer even when the project names a default app" },
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
  { name: "perf_mark", description: "Start a window of recorded frames: a later perf_report since=mark covers only frames after this.", inputSchema: obj({}) },
  { name: "perf_report", description: "Frame numbers over a window: for every field the app and Strata record (frame_ms, the app's own such as sim_ms, renderer_ms, gpu_ms, draw_calls, batch_instances, and the per-frame churn: instances, motions, commits, synced) the median, p90, mean of the slowest tenth and max; then findings in words: frames over twice the median and which fields rose in them, slots placed or meshes uploaded every frame, chunk-table churn, GPU-bound.",
    inputSchema: obj({ frames: int("the last n recorded frames (default 120)"), since: { type: "string", description: "\"mark\": the frames since perf_mark" }, from: num("game time in seconds (game_s) to start from"), to: num("game time in seconds to end at") }) },
  { name: "perf_values", description: "One field's raw values over a window (oldest first).", inputSchema: obj({ field: { type: "string", description: "the field (default frame_ms)" }, frames: int("the last n frames"), since: { type: "string" }, from: num("game_s from"), to: num("game_s to") }) },
  { name: "bench_compare", description: "Benchmark two builds of an app against each other: alternating runs (A B B A ...) so machine load hits both alike, each launched hidden with devtools, run for `frames` frames, then sampled over a window (frames from..to, or game time from_s..to_s on game_s). Answers each build's pooled median, p90, slowest-10% mean and max per field, the per-run medians, and the difference. Quits any running app first.",
    inputSchema: obj({ a: { type: "object", description: "build A: {bin, cwd, env}" }, b: { type: "object", description: "build B: {bin, cwd, env}" }, rounds: int("runs per build (default 6)"), frames: int("frames each run draws before sampling (default 160)"),
      window_from: int("first frame of the frame window (default 10)"), window_to: int("last frame (default frames)"), from_s: num("game-time window start (game_s); replaces the frame window"), to_s: num("game-time window end"),
      fields: { type: "array", items: { type: "string" }, description: "fields to compare (default frame_ms, sim_ms, renderer_ms, gpu_ms)" }, env: { type: "object", description: "STRATA_/UR_/MARCH_ switches for both builds" } }, ["a", "b"]) },
  { name: "profile", description: "Sample the running app for some seconds (macOS `sample`) and answer where its work goes: samples spent waiting are left out, every thread's work is counted together (or only the busiest thread's), heaviest functions by inclusive time, with each runtime function (allocation, memmove, native array sets, refcounting, drops) charged to the March functions that called it.",
    inputSchema: obj({ seconds: int("how long to sample (default 10)"), top: int("functions to list (default 30)"), thread: { type: "string", description: "\"all\" (default): every thread's work together, so a simulation on a worker counts; \"busiest\": only the thread with the most work" }, focus: { type: "array", items: { type: "string" }, description: "function name prefixes to attribute to callers (default: the runtime's allocation, copy, array-set, RC and drop functions)" } }) },
  { name: "screenshot_diff", description: "Compare two images pixel by pixel (PNG or PPM): how many pixels differ by more than a threshold, their bounding box and the largest difference, and optionally a diff image. With take=true, image b is the next frame the running app draws. Proves a render change left nothing else changed, or that a perturbation shows up.",
    inputSchema: obj({ a: { type: "string", description: "path of image a" }, b: { type: "string", description: "path of image b (or take=true)" }, take: { type: "boolean", description: "b is a fresh screenshot of the running app" }, threshold: int("per-channel difference that counts (default 0)"), save_diff: { type: "string", description: "write a diff image (changed pixels in red over a dimmed a) here" } }, ["a"]) },
];

const FORWARDED = new Set(["perf_mark", "perf_report", "perf_values", "status", "camera_get", "camera_set", "look_at", "scene", "light", "block_at", "raycast", "edit", "regenerate", "pause", "resume", "stats"]);

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
  if (name === "bench_compare") return text(await benchCompare(a));
  if (name === "profile") return text(await profile(a));
  if (name === "screenshot_diff") return text(await screenshotDiff(a));
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
      instructions: "Drive a running Strata app (the viewer, or the project's game): launch it, look (screenshot, camera), ask (block_at, raycast, stats), change (edit, light, regenerate), hold time (pause, step), read its output (log), quit. Measure: perf_report (frame numbers and findings), bench_compare (two builds, alternating runs), profile (where the main thread's time goes), screenshot_diff (did a change move any pixel). Positions are metres except edit, which is voxels. The viewer answers every command; a game answers the shared ones (status, pause, resume, step, screenshot, stats, perf_*) and its own." });
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
