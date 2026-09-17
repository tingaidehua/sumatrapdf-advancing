#!/usr/bin/env node
/**
 * Add a PDF or website URL into NotebookLM notebooks SumatraPDF1..N.
 * Re-running updates books.notebooklm to the latest notebook/url/source.
 * Usage:
 *   node notebooklm-add.mjs --pdf "D:\\a.pdf" --bookId 12
 *   node notebooklm-add.mjs --url "https://example.com" --bookId 12
 *   node notebooklm-add.mjs --job path\\to\\job.json
 */
import path from "node:path";
import fs from "node:fs";
import {
  connectCdp,
  notebookPage,
  ensureNotebook,
  countSources,
  addPdfSource,
  addWebsiteSource,
  selectOnlySource,
  clickByText,
} from "./lib/notebooklm.mjs";
import { finishJob, readJson, writeJson, bridgePaths, ensureJobDirs } from "./lib/bridge.mjs";
import { loadConfig } from "./lib/config.mjs";

function arg(name, fallback = null) {
  const i = process.argv.indexOf(`--${name}`);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return fallback;
}

function log(...a) {
  // Prefer file log so a hidden console isn't required for diagnostics.
  const line = a.map(String).join(" ");
  try {
    fs.appendFileSync(bridgePaths().bridgeLog, `[add] ${line}\n`);
  } catch {
    /* ignore */
  }
}

const CFG = loadConfig();
const SOURCES_PER_NOTEBOOK = Number(CFG.sourcesPerNotebook) || 50;
const MAX_NOTEBOOKS = Number(CFG.maxNotebooks) || 40;
const NOTEBOOK_PREFIX = CFG.notebookNamePrefix || "SumatraPDF";

function extractPrior(job) {
  const raw = job.notebooklm;
  if (!raw) return {};
  if (typeof raw === "object") return raw;
  try {
    return JSON.parse(raw);
  } catch {
    return {};
  }
}

async function pickNotebook(page, prior = {}) {
  if (prior.notebookUrl) {
    await page.goto(prior.notebookUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
    await page.waitForTimeout(1000);
    return {
      name: prior.notebook || "SumatraPDF?",
      count: await countSources(page),
      url: page.url(),
      reused: true,
    };
  }
  if (prior.notebook) {
    await page.goto("https://notebook.google.com/", { waitUntil: "domcontentloaded", timeout: 60000 });
    await page.waitForTimeout(800);
    if (await ensureNotebook(page, prior.notebook)) {
      return { name: prior.notebook, count: await countSources(page), url: page.url(), reused: true };
    }
  }
  for (let n = 1; n <= MAX_NOTEBOOKS; n++) {
    const name = `${NOTEBOOK_PREFIX}${n}`;
    await page.goto("https://notebook.google.com/", {
      waitUntil: "domcontentloaded",
      timeout: 60000,
    });
    await page.waitForTimeout(1000);
    const ok = await ensureNotebook(page, name);
    if (!ok) continue;
    const count = await countSources(page);
    if (count < SOURCES_PER_NOTEBOOK + 1) {
      return { name, count, url: page.url() };
    }
  }
  throw new Error(`no free ${NOTEBOOK_PREFIX}* notebook slot`);
}

function selectKey(pdfPath, sourceUrl, title, upload) {
  if (upload?.sourceTitle) return upload.sourceTitle;
  if (title) return title;
  if (pdfPath) return path.basename(pdfPath);
  try {
    return new URL(sourceUrl).hostname.replace(/^www\./, "");
  } catch {
    return sourceUrl || "";
  }
}

async function main() {
  ensureJobDirs();
  const jobPath = arg("job");
  const job = jobPath ? readJson(jobPath, {}) : {};
  const pdfPath = arg("pdf", job.pdfPath || null);
  const sourceUrl = arg("url", job.sourceUrl || job.webUrl || null);
  const title = arg("title", job.title || "");
  const bookId = Number(arg("bookId", job.bookId || 0));
  if (!pdfPath && !sourceUrl) {
    throw new Error("missing --pdf/--url / job.pdfPath/job.sourceUrl");
  }
  const prior = extractPrior(job);

  const { browser, context, port } = await connectCdp();
  log(`CDP ${port} pdf=${pdfPath || ""} url=${sourceUrl || ""}`);
  try {
    const page = await notebookPage(context);
    const nb = await pickNotebook(page, prior);
    const upload = sourceUrl
      ? await addWebsiteSource(page, sourceUrl, title)
      : await addPdfSource(page, pdfPath);
    const key = selectKey(pdfPath, sourceUrl, title, upload);
    // Always leave exactly this source selected (not 全选).
    await selectOnlySource(page, key);
    await clickByText(page, ["^对话$", "对话", "Chat"], 3000);

    const result = {
      action: "notebooklm.add",
      ok: !!upload.ok || !!upload.alreadyPresent,
      bookId,
      pdfPath: pdfPath || "",
      sourceUrl: sourceUrl || "",
      notebook: nb.name,
      notebookUrl: page.url(),
      sourceTitle: upload.sourceTitle || key,
      sourceCountBefore: nb.count,
      upload,
      reusedNotebook: !!nb.reused,
      updatedMs: Date.now(),
    };
    // Host reads jobs/done and rewrites books.notebooklm — safe to re-run.
    writeJson(path.join(bridgePaths().jobsDone, `result-add-${Date.now()}.json`), result);
    if (jobPath) {
      finishJob(jobPath, !!result.ok, result);
    }
    process.stdout.write(JSON.stringify(result) + "\n");
    if (!result.ok) process.exitCode = 1;
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
  log(String(e));
  process.exit(1);
});
