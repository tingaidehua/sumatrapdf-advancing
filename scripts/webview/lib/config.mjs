import path from "node:path";
import { fileURLToPath } from "node:url";
import { readJson } from "./bridge.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const defaults = {
  sourcesPerNotebook: 50,
  maxNotebooks: 40,
  notebookNamePrefix: "SumatraPDF",
  cdpPreferredPort: 9224,
  uploadMaxMb: 200,
};

/** Load scripts/webview/config.json (editable via 脚本管理). Immediate on next run. */
export function loadConfig() {
  const file = path.join(__dirname, "..", "config.json");
  const raw = readJson(file, {});
  return { ...defaults, ...(raw && typeof raw === "object" ? raw : {}) };
}
