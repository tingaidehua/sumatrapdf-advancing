/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct LibraryStore;

enum class LibrarySort {
    OpenCount,
    ReadingTime,
    Recent,
    Title,
};

enum class LibraryBookScope {
    All,
    Desk,
    Unclassified,
    Collection,
    ManualRoot,
};

struct LibraryBook {
    i64 id = 0;
    Str path;
    Str title;
    i64 openCount = 0;
    i64 readingSeconds = 0;
    i64 lastReadMs = 0;
};

struct LibraryCollection {
    i64 id = 0;
    i64 parentId = 0;
    bool isShelf = false;
    Str name;
};

struct LibraryPathChange {
    i64 bookId = 0;
    Str oldPath;
    Str newPath;
    bool targetExists = false;
    bool conflict = false;
};

LibraryStore* LibraryStoreOpen(Str dbPath);
void LibraryStoreClose(LibraryStore* store);
bool LibraryStoreIsOpen(LibraryStore* store);
Str LibraryStoreError(LibraryStore* store);

LibraryBook* LibraryStoreRecordOpen(LibraryStore* store, Str path, Str title, i64 nowMs, bool* placedAtRoot = nullptr);
LibraryBook* LibraryStoreAddBook(LibraryStore* store, Str path, Str title, i64 nowMs);
LibraryBook* LibraryStoreImportBook(LibraryStore* store, Str path, Str title, i64 openCount, i64 nowMs);
bool LibraryStoreAddReadingTime(LibraryStore* store, Str path, i64 seconds, i64 nowMs);
Vec<LibraryBook*> LibraryStoreGetBooks(LibraryStore* store, LibraryBookScope scope, i64 collectionId, LibrarySort sort,
                                       Str filter);
Vec<LibraryCollection*> LibraryStoreGetCollections(LibraryStore* store);
LibraryCollection* LibraryStoreCreateCollection(LibraryStore* store, i64 parentId, bool isShelf, Str name);
bool LibraryStoreDeleteCollection(LibraryStore* store, i64 collectionId);
bool LibraryStoreRenameCollection(LibraryStore* store, i64 collectionId, Str name);
bool LibraryStoreMoveCollection(LibraryStore* store, i64 collectionId, i64 newParentId);
bool LibraryStoreAddBookToCollection(LibraryStore* store, i64 bookId, i64 collectionId);
// Collection id 0 denotes the visible Library root. A move removes the source
// membership; a copy preserves it and adds the destination membership.
bool LibraryStorePlaceBook(LibraryStore* store, i64 bookId, i64 sourceCollectionId, i64 targetCollectionId, bool copy);
bool LibraryStoreSetBookOnDesk(LibraryStore* store, i64 bookId, bool onDesk);
bool LibraryStoreRemoveBook(LibraryStore* store, i64 bookId);

Vec<LibraryPathChange*> LibraryStorePreviewPathReplace(LibraryStore* store, Str oldPrefix, Str newPrefix);
bool LibraryStoreApplyPathReplace(LibraryStore* store, Vec<LibraryPathChange*>& changes);

void DeleteLibraryBook(LibraryBook* book);
void DeleteLibraryBooks(Vec<LibraryBook*>& books);
void DeleteLibraryCollection(LibraryCollection* collection);
void DeleteLibraryCollections(Vec<LibraryCollection*>& collections);
void DeleteLibraryPathChange(LibraryPathChange* change);
void DeleteLibraryPathChanges(Vec<LibraryPathChange*>& changes);

#if defined(DEBUG)
void LibraryStore_UnitTests();
#endif
