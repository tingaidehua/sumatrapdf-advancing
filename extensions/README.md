# SumatraPDF browsers / agents — extensions

Unified plugin root for WebView2 (`Browser-AIChat`, `Browser-Library`) and future agents.

## Layout

- `installed/<id>/plugin.json` — **host** plugins (native / Playwright actions)
- `installed/<id>/manifest.json` — unpacked Chromium / WebView2 extensions (`AddBrowserExtension`)

Host `plugin.json` field `browser`:

- `Browser-AIChat` — only AI panel puzzle menu (CDP 9224)
- `Browser-Library` — only center Web puzzle menu (CDP 9225)

Built-in NotebookLM host plugins are **Browser-AIChat only** and operate on the **center pane** (PDF or webpage).

Runtime copy lives at `%OneDrive%\SumatraPDF\extensions\` (seeded on launch).

Repo: https://github.com/tingaidehua/sumatrapdf-browsers-agents
