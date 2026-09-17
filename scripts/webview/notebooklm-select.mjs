#!/usr/bin/env node
/**
 * Select a single source in the open NotebookLM notebook (via CDP).
 * Usage:
 *   node notebooklm-select.mjs --title "a.pdf" [--url URL]
 *   node notebooklm-select.mjs --job path\\to\\select-job.json
 */
import {
  connectCdp,
  notebookPage,
  selectOnlySource,
  clickByText,
} from "./lib/notebooklm.mjs";
import { finishJob, readJson, ensureJobDirs } from "./lib/bridge.mjs";

function arg(name, fallback = null) {
  const i = process.argv.indexOf(`--${name}`);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return fallback;
}

async function main() {
  ensureJobDirs();
  const jobPath = arg("job");
  const job = jobPath ? readJson(jobPath, {}) : {};
  const title = arg("title", job.sourceTitle || job.fileName);
  const url = arg("url", job.notebookUrl);
  if (!title) {
    throw new Error("missing --title / job.sourceTitle");
  }

  const { browser, context, port } = await connectCdp();
  // Log to bridge.log (hidden spawn has no console).
  try {
    const fs = await import("node:fs");
    const { bridgePaths } = await import("./lib/bridge.mjs");
    fs.appendFileSync(
      bridgePaths().bridgeLog,
      `[select] CDP ${port} title=${title}\n`,
    );
  } catch {
    /* ignore */
  }
  try {
    const page = await notebookPage(context);
    if (url) {
      const cur = page.url();
      if (!cur.includes(String(url).replace(/\/$/, "").split("/").pop() || "___")) {
        await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
        await page.waitForTimeout(1200);
      }
    }
    await clickByText(page, ["^来源$", "Sources", "来源"], 4000);
    await selectOnlySource(page, title);
    await clickByText(page, ["^对话$", "对话", "^Chat$", "Chat"], 4000);
    const result = { ok: true, action: "notebooklm.select", title, url: page.url() };
    if (jobPath) {
      finishJob(jobPath, true, result);
    }
    console.log(JSON.stringify(result));
  } catch (err) {
    if (jobPath) {
      finishJob(jobPath, false, { error: String(err) });
    }
    throw err;
  } finally {
    await browser.close().catch(() => {});
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
