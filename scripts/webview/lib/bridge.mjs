import fs from "node:fs";
import path from "node:path";
import os from "node:os";

export function oneDriveSumatraDir() {
  const od = process.env.OneDrive || path.join(os.homedir(), "OneDrive");
  return path.join(od, "SumatraPDF");
}

/** Prefer structured layout; fall back to legacy flat files during migration. */
function prefer(newPath, legacyPath) {
  if (fs.existsSync(newPath)) return newPath;
  if (legacyPath && fs.existsSync(legacyPath)) return legacyPath;
  return newPath;
}

export function bridgePaths() {
  const root = oneDriveSumatraDir();
  const webPanel = path.join(root, "WebPanel");
  const bridgeNew = path.join(webPanel, "bridge", "web-bridge.json");
  const bridgeLegacy = path.join(webPanel, "web-bridge.json");
  return {
    root,
    webPanel,
    // Canonical structured tree under WebPanel/
    bridge: prefer(bridgeNew, bridgeLegacy),
    bridgeCanonical: bridgeNew,
    jobsPending: path.join(webPanel, "jobs", "pending"),
    jobsDone: path.join(webPanel, "jobs", "done"),
    jobsFailed: path.join(webPanel, "jobs", "failed"),
    tabsIndex: prefer(path.join(webPanel, "tabs", "index.json"), path.join(webPanel, "tabs.json")),
    pdfTabMap: prefer(path.join(webPanel, "tabs", "pdf-map.json"), path.join(webPanel, "pdf-tabs.json")),
    profileDir: prefer(path.join(webPanel, "profile", "WebView2"), path.join(webPanel, "WebView2")),
    faviconsDir: prefer(path.join(webPanel, "cache", "favicons"), path.join(webPanel, "favicons")),
    bridgeLog: prefer(path.join(webPanel, "bridge", "bridge.log"), path.join(webPanel, "bridge.log")),
  };
}

export function readJson(file, fallback = null) {
  try {
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch {
    return fallback;
  }
}

export function writeJson(file, obj) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, JSON.stringify(obj, null, 2), "utf8");
}

export function ensureJobDirs() {
  const p = bridgePaths();
  for (const d of [p.jobsPending, p.jobsDone, p.jobsFailed]) {
    fs.mkdirSync(d, { recursive: true });
  }
  // Keep bridge writes on the canonical path.
  fs.mkdirSync(path.dirname(p.bridgeCanonical), { recursive: true });
  return p;
}

export function listPendingJobs() {
  const p = ensureJobDirs();
  return fs
    .readdirSync(p.jobsPending)
    .filter((n) => n.endsWith(".json"))
    .map((n) => path.join(p.jobsPending, n))
    .sort();
}

export function finishJob(jobPath, ok, result) {
  const p = bridgePaths();
  const base = path.basename(jobPath);
  const dest = path.join(ok ? p.jobsDone : p.jobsFailed, base);
  writeJson(dest, { ...readJson(jobPath, {}), result, finishedAt: Date.now(), ok });
  try {
    fs.unlinkSync(jobPath);
  } catch {
    /* ignore */
  }
  return dest;
}
