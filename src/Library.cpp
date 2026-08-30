/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/Win.h"

#include "gui/UIModels.h"

#include "Settings.h"
#include "AppTools.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "MainWindow.h"
#include "Notifications.h"
#include "WindowTab.h"
#include "LibraryReadingTracker.h"
#include "LibraryStore.h"

#include "Library.h"
#include "SumatraConfig.h"
#include "SumatraLog.h"

static LibraryStore* gLibraryStore = nullptr;
static LibraryReadingTracker gReadingTracker;

static bool IsPdfPath(Str path) {
    return str::EndsWithI(path, StrL(".pdf"));
}

void LibraryInitialize() {
    if (gLibraryStore) {
        return;
    }
    TempStr dbPath;
    if (gForTesting) {
        TempStr fromEnv = GetEnvVariableTemp(StrL("SUMATRA_LIBRARY_DB"));
        dbPath = fromEnv ? fromEnv
                         : path::JoinTemp(GetTempDirTemp(), fmt("sumatra-library-test-%lu.db", GetCurrentProcessId()));
        logf("LibraryInitialize: test db '%s'\n", dbPath);
    } else {
        dbPath = GetPathInAppDataDirTemp(StrL("SumatraPDF-library.db"));
    }
    if (!gLogFilePath) {
        StartLogToFile(GetPathInAppDataDirTemp(StrL("library-debug.log")), false);
    }
    gLibraryStore = LibraryStoreOpen(dbPath);
    if (!LibraryStoreIsOpen(gLibraryStore)) {
        logf("Library disabled: %s\n", LibraryStoreError(gLibraryStore));
        MaybeDelayedWarningNotification(fmt("Library is disabled because SumatraPDF-library.db could not be opened: %s",
                                            LibraryStoreError(gLibraryStore)));
    }
}

void LibraryShutdown() {
    LibraryUpdateReadingActivity();
    LibraryReadingTrackerReset(&gReadingTracker);
    LibraryStoreClose(gLibraryStore);
    gLibraryStore = nullptr;
}

bool LibraryIsAvailable() {
    return LibraryStoreIsOpen(gLibraryStore);
}

Str LibraryGetError() {
    return LibraryStoreError(gLibraryStore);
}

LibraryStore* LibraryGetStore() {
    return gLibraryStore;
}

bool LibraryRecordOpenedDocument(Str path, Str title, bool* treeChanged) {
    if (treeChanged) {
        *treeChanged = false;
    }
    if (!LibraryIsAvailable() || !IsPdfPath(path)) {
        return false;
    }
    bool placedAtRoot = false;
    LibraryBook* book = LibraryStoreRecordOpen(gLibraryStore, path, title, UnixTimeMsNow(), &placedAtRoot);
    if (!book) {
        logf("Library failed to record opened PDF: '%s': %s\n", path, LibraryStoreError(gLibraryStore));
        return false;
    }
    logf("LibraryRecordOpened: path='%s' id=%lld placedAtRoot=%d\n", path, book->id, placedAtRoot ? 1 : 0);
    if (treeChanged) {
        *treeChanged = placedAtRoot;
    }
    DeleteLibraryBook(book);
    return true;
}

static Str ActiveReadingPath() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || IsIconic(foreground)) {
        return Str();
    }
    for (MainWindow* win : gWindows) {
        if (win->hwndFrame != foreground || win->isBeingClosed) {
            continue;
        }
        WindowTab* tab = win->CurrentTab();
        if (tab && tab->IsDocLoaded() && IsPdfPath(tab->filePath)) {
            return tab->filePath;
        }
    }
    return Str();
}

void LibraryUpdateReadingActivity() {
    if (!LibraryIsAvailable()) {
        return;
    }
    Str activePath = ActiveReadingPath();
    LibraryReadingSettlement settlement = LibraryReadingTrackerUpdate(&gReadingTracker, activePath, GetTickCount64());
    if (settlement.seconds > 0) {
        LibraryStoreAddReadingTime(gLibraryStore, settlement.path, settlement.seconds, UnixTimeMsNow());
    }
    LibraryReadingSettlementFree(&settlement);
}
