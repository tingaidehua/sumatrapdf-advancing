#!/usr/bin/env node
/**
 * Ask a question in the currently open NotebookLM notebook (via CDP).
 * Usage:
 *   node notebooklm-chat.mjs --q "这本书讲什么？"
 *   node notebooklm-chat.mjs --url URL --q "..."
 */
import { connectCdp, notebookPage, askChat, clickByText } from "./lib/notebooklm.mjs";

function arg(name, fallback = null) {
  const i = process.argv.indexOf(`--${name}`);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return fallback;
}

async function main() {
  const q = arg("q", arg("question", "用一句话概括这份资料的核心主题"));
  const url = arg("url");
  const { browser, context, port } = await connectCdp();
  console.error(`[notebooklm-chat] CDP ${port}`);
  try {
    const page = await notebookPage(context);
    if (url) {
      await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
      await page.waitForTimeout(1200);
    }
    // Best-effort open Chat studio
    await clickByText(page, ["Chat", "聊天"], 3000);
    const result = await askChat(page, q);
    console.log(JSON.stringify(result, null, 2));
    if (!result.ok) process.exitCode = 1;
  } finally {
    await browser.close().catch(() => {});
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
