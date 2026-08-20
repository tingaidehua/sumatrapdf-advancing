/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct LibraryReadingTracker {
    Str path;
    u64 startedAtMs = 0;
    u64 remainderMs = 0;
};

struct LibraryReadingSettlement {
    Str path;
    i64 seconds = 0;
};

void LibraryReadingTrackerReset(LibraryReadingTracker*);
LibraryReadingSettlement LibraryReadingTrackerUpdate(LibraryReadingTracker*, Str activePath, u64 monotonicMs);
void LibraryReadingSettlementFree(LibraryReadingSettlement*);

#if defined(DEBUG)
void LibraryReadingTracker_UnitTests();
#endif
