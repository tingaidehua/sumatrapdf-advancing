#!/usr/bin/env node
/**
 * SumatraPDF WebView bridge CLI — AI/Cursor automation surface.
 *
 * Usage:
 *   node cli.mjs status
 *   node cli.mjs cdp
 *   node cli.mjs drain
 *   node cli.mjs add --pdf "D:\\a.pdf" --bookId 12
 *   node cli.mjs select --title "a.pdf" [--url URL]
 *   node cli.mjs chat --q "这本书讲什么？"
 *   node cli.mjs test-add --pdf PATH
 *   node cli.mjs flywheel [--pdf PATH] [--bookId N] [--skip-build] [--reuse]
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  bridgePaths,
  ensureJobDirs,
  listPendingJobs,
  readJson,
  writeJson,
  finishJob,
} from "./lib/bridge.mjs";
import { probeCdpPorts } from "./lib/notebooklm.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function arg(name, fallback = null) {
  const i = process.argv.indexOf(`--${name}`);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return fallback;
}

function cmd() {
  return process.argv[2] || "status";
}

async function probeCdp() {
  return probeCdpPorts(9224);
}

async function status() {
  const p = ensureJobDirs();
  const cdp = await probeCdp();
  const pending = listPendingJobs();
  const done = fs.existsSync(p.jobsDone)
    ? fs.readdirSync(p.jobsDone).filter((n) => n.endsWith(".json"))
    : [];
  const failed = fs.existsSync(p.jobsFailed)
    ? fs.readdirSync(p.jobsFailed).filter((n) => n.endsWith(".json"))
    : [];
  const out = {
    bridgePath: p.bridge,
    bridge: cdp.bridge,
    cdp: cdp.live,
    cdpBest: cdp.best,
    pending: pending.map((f) => ({ file: f, job: readJson(f) })),
    doneCount: done.length,
    failedCount: failed.length,
    playwright: fs.existsSync(path.join(__dirname, "node_modules", "playwright", "package.json")),
  };
  console.log(JSON.stringify(out, null, 2));
  return out;
}

function run(spawn, script, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script, ...args], {
      cwd: __dirname,
      stdio: "inherit",
      windowsHide: true,
      env: { ...process.env },
    });
    child.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`exit ${code}`))));
  });
}

async function drain() {
  const { spawn } = await import("node:child_process");
  const pending = listPendingJobs();
  const report = [];
  for (const jobPath of pending) {
    const job = readJson(jobPath, {});
    const action = job.action || "";
    try {
      if (action === "notebooklm.add") {
        await run(spawn, path.join(__dirname, "notebooklm-add.mjs"), ["--job", jobPath]);
        report.push({ jobPath, ok: true, action });
      } else if (action === "notebooklm.select") {
        const args = [];
        if (job.sourceTitle || job.fileName) args.push("--title", job.sourceTitle || job.fileName);
        if (job.notebookUrl) args.push("--url", job.notebookUrl);
        await run(spawn, path.join(__dirname, "notebooklm-select.mjs"), args);
        finishJob(jobPath, true, { action, ok: true });
        report.push({ jobPath, ok: true, action });
      } else {
        finishJob(jobPath, false, { error: `unknown action ${action}` });
        report.push({ jobPath, ok: false, action, error: "unknown" });
      }
    } catch (e) {
      if (fs.existsSync(jobPath)) {
        finishJob(jobPath, false, { error: String(e) });
      }
      report.push({ jobPath, ok: false, action, error: String(e) });
    }
  }
  console.log(JSON.stringify({ drained: report.length, report }, null, 2));
}

async function add() {
  const pdf = arg("pdf");
  const bookId = arg("bookId", "0");
  if (!pdf) throw new Error("--pdf required");
  const p = ensureJobDirs();
  const jobPath = path.join(p.jobsPending, `add-cli-${Date.now()}.json`);
  writeJson(jobPath, {
    action: "notebooklm.add",
    bookId: Number(bookId),
    pdfPath: pdf,
    title: path.basename(pdf),
  });
  const { spawn } = await import("node:child_process");
  await run(spawn, path.join(__dirname, "notebooklm-add.mjs"), ["--job", jobPath]);
  console.log(JSON.stringify({ ok: true, jobPath }, null, 2));
}

async function select() {
  const title = arg("title");
  const url = arg("url");
  if (!title) throw new Error("--title required");
  const { spawn } = await import("node:child_process");
  const args = ["--title", title];
  if (url) args.push("--url", url);
  await run(spawn, path.join(__dirname, "notebooklm-select.mjs"), args);
}

async function chat() {
  const { spawn } = await import("node:child_process");
  const args = [];
  const q = arg("q", arg("question"));
  if (q) args.push("--q", q);
  const url = arg("url");
  if (url) args.push("--url", url);
  await run(spawn, path.join(__dirname, "notebooklm-chat.mjs"), args);
}

async function testAdd() {
  const cdp = await probeCdp();
  if (!cdp.best) {
    throw new Error("no live CDP — start SumatraPDF with WebPanel open first");
  }
  const bridge = {
    ...(cdp.bridge || {}),
    cdpPort: cdp.best.port,
    cdpEndpoint: `http://127.0.0.1:${cdp.best.port}`,
  };
  writeJson(bridgePaths().bridgeCanonical, bridge);
  console.error(`[test-add] using CDP ${cdp.best.port} (${cdp.best.browser})`);
  await add();
}

async function flywheel() {
  const { spawn } = await import("node:child_process");
  const pass = process.argv.slice(3);
  await run(spawn, path.join(__dirname, "flywheel.mjs"), pass);
}

const handlers = {
  status,
  cdp: async () => console.log(JSON.stringify(await probeCdp(), null, 2)),
  drain,
  add,
  select,
  chat,
  "test-add": testAdd,
  flywheel,
};

const fn = handlers[cmd()];
if (!fn) {
  console.error(`unknown command: ${cmd()}`);
  console.error("commands: " + Object.keys(handlers).join(", "));
  process.exit(2);
}
fn().catch((e) => {
  console.error(e);
  process.exit(1);
});
