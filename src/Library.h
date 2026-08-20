/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct LibraryStore;
struct MainWindow;

void LibraryInitialize();
void LibraryShutdown();
bool LibraryIsAvailable();
Str LibraryGetError();
LibraryStore* LibraryGetStore();

bool LibraryRecordOpenedDocument(Str path, Str title);
void LibraryUpdateReadingActivity();
