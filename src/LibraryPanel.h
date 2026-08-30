/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

// Left inset of the library pane so the frame's HTLEFT hit-test is reachable.
constexpr int kLibraryLeftGutterDip = 10;

void CreateLibraryPanel(MainWindow* win);
void LayoutLibraryPanel(MainWindow* win);
void UpdateLibraryPanelText(MainWindow* win);
void RefreshLibraryPanel(MainWindow* win);
void RefreshLibraryPanels();
void SyncLibrarySelection(MainWindow* win);
void SetLibraryPanelVisible(MainWindow* win, bool visible);
TempStr LibraryDbgControlTemp(Str action, Str a, Str b, int n1, int n2, int* exitCodeOut);
