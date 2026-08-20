/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "LibraryReadingTracker.h"

void LibraryReadingTrackerReset(LibraryReadingTracker* tracker) {
    if (!tracker) return;
    str::FreePtr(&tracker->path);
    tracker->startedAtMs = 0;
    tracker->remainderMs = 0;
}

void LibraryReadingSettlementFree(LibraryReadingSettlement* settlement) {
    if (!settlement) return;
    str::FreePtr(&settlement->path);
    settlement->seconds = 0;
}

LibraryReadingSettlement LibraryReadingTrackerUpdate(LibraryReadingTracker* tracker, Str activePath, u64 monotonicMs) {
    LibraryReadingSettlement result;
    if (!tracker) return result;

    bool samePath = activePath && tracker->path && str::EqI(activePath, tracker->path);
    bool stopCurrent = tracker->startedAtMs != 0 && (!activePath || !samePath);
    if (stopCurrent) {
        u64 elapsed = monotonicMs >= tracker->startedAtMs ? monotonicMs - tracker->startedAtMs : 0;
        u64 totalMs = tracker->remainderMs + elapsed;
        result.seconds = (i64)(totalMs / 1000);
        tracker->remainderMs = totalMs % 1000;
        if (result.seconds > 0) result.path = str::Dup(tracker->path);
        tracker->startedAtMs = 0;
    }

    if (activePath && !samePath) {
        str::ReplaceWithCopy(&tracker->path, activePath);
        tracker->remainderMs = 0;
        samePath = true;
    }
    if (activePath && samePath && tracker->startedAtMs == 0) {
        tracker->startedAtMs = monotonicMs;
    }
    return result;
}

#if defined(DEBUG)
#include "base/UtAssert.h"

void LibraryReadingTracker_UnitTests() {
    LibraryReadingTracker tracker;
    LibraryReadingSettlement s = LibraryReadingTrackerUpdate(&tracker, StrL("C:\\Books\\A.pdf"), 1000);
    utassert(!s.path && s.seconds == 0);
    s = LibraryReadingTrackerUpdate(&tracker, {}, 2550);
    utassert(str::Eq(s.path, StrL("C:\\Books\\A.pdf")) && s.seconds == 1 && tracker.remainderMs == 550);
    LibraryReadingSettlementFree(&s);
    s = LibraryReadingTrackerUpdate(&tracker, StrL("c:\\books\\a.pdf"), 3000);
    utassert(!s.path && tracker.startedAtMs == 3000);
    s = LibraryReadingTrackerUpdate(&tracker, {}, 3500);
    utassert(str::EqI(s.path, StrL("C:\\Books\\A.pdf")) && s.seconds == 1 && tracker.remainderMs == 50);
    LibraryReadingSettlementFree(&s);
    s = LibraryReadingTrackerUpdate(&tracker, StrL("C:\\Books\\A.pdf"), 4000);
    LibraryReadingSettlementFree(&s);
    s = LibraryReadingTrackerUpdate(&tracker, StrL("C:\\Books\\B.pdf"), 5250);
    utassert(str::EqI(s.path, StrL("C:\\Books\\A.pdf")) && s.seconds == 1);
    utassert(str::EqI(tracker.path, StrL("C:\\Books\\B.pdf")) && tracker.remainderMs == 0);
    LibraryReadingSettlementFree(&s);
    s = LibraryReadingTrackerUpdate(&tracker, {}, 5000);
    utassert(!s.path && s.seconds == 0);
    LibraryReadingTrackerReset(&tracker);
}
#endif
