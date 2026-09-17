/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

void CreateWebPanel(MainWindow* win);
void DestroyWebPanel(MainWindow* win);
void RelayoutWebPanel(MainWindow* win);
void OnWebPanelToggle(MainWindow* win);
void CloseWebPanel(MainWindow* win);
void WebPanelOnDocumentChanged(MainWindow* win);
void UpdateWebPanelDpi(MainWindow* win, int dpi);
void UpdateWebPanelTheme(MainWindow* win);
bool IsWebPanelVisible(MainWindow* win);
// Queue a NotebookLM add job and spawn the Node/Playwright helper (CDP 9224).
void WebPanelAddPdfToNotebookLm(MainWindow* win, i64 bookId, Str pdfPath, Str title);
// Select only this PDF's source in NotebookLM (uncheck others) via Playwright.
void WebPanelSelectNotebookLmSource(MainWindow* win, i64 bookId, Str pdfPath, Str title);
// Clear per-PDF WebPanel tab id / url bindings (library context menu).
void WebPanelClearPdfTabIds(i64 bookId);
void WebPanelClearPdfTabUrls(i64 bookId);
void WebPanelPollBridgeResults();
TempStr ScriptsWebviewDirTemp();
void WebPanelSpawnScript(Str scriptName, Str extraArgs = {});
// -dbg-control TestWebPanel: show|hide|status|add|poll (AI/Cursor closed loop).
TempStr WebPanelDbgControlTemp(Str action, Str a, Str b, int n1, int n2, int* exitCodeOut);
