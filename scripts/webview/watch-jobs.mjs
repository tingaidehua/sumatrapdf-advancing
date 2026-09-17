#!/usr/bin/env node
/** Poll WebPanel/jobs/pending and run notebooklm actions. */
import fs from "node:fs";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { listPendingJobs, readJson, ensureJobDirs, finishJob } from "./lib/bridge.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function runNode(script, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script, ...args], {
      cwd: __dirname,
      stdio: "inherit",
      windowsHide: true,
    });
    child.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`exit ${code}`))));
  });
}

async function handle(jobPath) {
  const job = readJson(jobPath, {});
  const action = job.action || "";
  try {
    if (action === "notebooklm.add") {
      await runNode(path.join(__dirname, "notebooklm-add.mjs"), ["--job", jobPath]);
      return;
    }
    if (action === "notebooklm.select") {
      const args = ["--title", job.sourceTitle || job.fileName || ""];
      if (job.notebookUrl) args.push("--url", job.notebookUrl);
      await runNode(path.join(__dirname, "notebooklm-select.mjs"), args);
      finishJob(jobPath, true, { action, ok: true });
      return;
    }
    finishJob(jobPath, false, { error: `unknown action: ${action}` });
  } catch (e) {
    finishJob(jobPath, false, { error: String(e) });
  }
}

ensureJobDirs();
console.log("watching NotebookLM jobs…");
setInterval(async () => {
  for (const job of listPendingJobs()) {
    // skip if already being handled — rename lock
    const lock = job + ".lock";
    try {
      fs.renameSync(job, lock);
    } catch {
      continue;
    }
    await handle(lock);
  }
}, 1000);
