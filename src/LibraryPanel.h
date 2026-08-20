/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

void CreateLibraryPanel(MainWindow* win);
void LayoutLibraryPanel(MainWindow* win);
void UpdateLibraryPanelText(MainWindow* win);
void RefreshLibraryPanel(MainWindow* win);
void RefreshLibraryPanels();
void SetLibraryPanelVisible(MainWindow* win, bool visible);
