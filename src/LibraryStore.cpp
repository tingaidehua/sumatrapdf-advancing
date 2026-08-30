/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/Win.h"

#include "sqlite3.h"

#include "LibraryStore.h"
#include "SumatraLog.h"

struct LibraryStore {
    sqlite3* db = nullptr;
    Str error;
};

static void SetError(LibraryStore* store, Str context) {
    if (!store) {
        return;
    }
    const char* msg = store->db ? sqlite3_errmsg(store->db) : "database is not open";
    str::ReplaceWithCopy(&store->error, fmt("%s: %s", context, Str(msg)));
    logf("LibraryStore: %s\n", store->error);
}

static bool Exec(LibraryStore* store, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(store->db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return true;
    }
    Str detail = err ? Str(err) : Str(sqlite3_errmsg(store->db));
    str::ReplaceWithCopy(&store->error, detail);
    logf("LibraryStore SQL error: %s\n", detail);
    sqlite3_free(err);
    return false;
}

static TempStr NormalizePathTemp(Str path) {
    return path::NormalizeTemp(path);
}

static Str PathKey(Str path) {
    TempStr normalized = NormalizePathTemp(path);
    return str::ToLower(normalized);
}

static void BindText(sqlite3_stmt* stmt, int col, Str value) {
    sqlite3_bind_text(stmt, col, value.s, len(value), SQLITE_TRANSIENT);
}

static sqlite3_stmt* Prepare(LibraryStore* store, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetError(store, StrL("prepare"));
        return nullptr;
    }
    return stmt;
}

static Str ColumnTextDup(sqlite3_stmt* stmt, int col) {
    const char* s = (const char*)sqlite3_column_text(stmt, col);
    int n = sqlite3_column_bytes(stmt, col);
    return s ? str::Dup(Str(s, n)) : Str();
}

static LibraryBook* ReadBook(sqlite3_stmt* stmt) {
    auto* book = new LibraryBook();
    book->id = sqlite3_column_int64(stmt, 0);
    book->path = ColumnTextDup(stmt, 1);
    book->title = ColumnTextDup(stmt, 2);
    book->openCount = sqlite3_column_int64(stmt, 3);
    book->readingSeconds = sqlite3_column_int64(stmt, 4);
    book->lastReadMs = sqlite3_column_int64(stmt, 5);
    return book;
}

void DeleteLibraryBook(LibraryBook* book) {
    if (!book) {
        return;
    }
    str::Free(book->path);
    str::Free(book->title);
    delete book;
}

void DeleteLibraryBooks(Vec<LibraryBook*>& books) {
    for (LibraryBook* book : books) {
        DeleteLibraryBook(book);
    }
    books.Reset();
}

void DeleteLibraryCollection(LibraryCollection* collection) {
    if (!collection) {
        return;
    }
    str::Free(collection->name);
    delete collection;
}

void DeleteLibraryCollections(Vec<LibraryCollection*>& collections) {
    for (LibraryCollection* collection : collections) {
        DeleteLibraryCollection(collection);
    }
    collections.Reset();
}

void DeleteLibraryPathChange(LibraryPathChange* change) {
    if (!change) {
        return;
    }
    str::Free(change->oldPath);
    str::Free(change->newPath);
    delete change;
}

void DeleteLibraryPathChanges(Vec<LibraryPathChange*>& changes) {
    for (LibraryPathChange* change : changes) {
        DeleteLibraryPathChange(change);
    }
    changes.Reset();
}

static bool CreateSchema(LibraryStore* store) {
    const char* sql = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS books (
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL,
  path_key TEXT NOT NULL UNIQUE,
  title TEXT NOT NULL,
  open_count INTEGER NOT NULL DEFAULT 0 CHECK(open_count >= 0),
  reading_seconds INTEGER NOT NULL DEFAULT 0 CHECK(reading_seconds >= 0),
  last_read_ms INTEGER NOT NULL DEFAULT 0,
  created_ms INTEGER NOT NULL,
  updated_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS collections (
  id INTEGER PRIMARY KEY,
  parent_id INTEGER REFERENCES collections(id) ON DELETE CASCADE,
  kind INTEGER NOT NULL CHECK(kind IN (1, 2)),
  name TEXT NOT NULL,
  created_ms INTEGER NOT NULL,
  UNIQUE(parent_id, name COLLATE NOCASE)
);
CREATE TABLE IF NOT EXISTS book_collections (
  book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE,
  collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE,
  added_ms INTEGER NOT NULL,
  PRIMARY KEY(book_id, collection_id)
);
CREATE TABLE IF NOT EXISTS desk_books (
  book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE,
  added_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS manual_books (
  book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE,
  added_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_collections_parent ON collections(parent_id);
CREATE INDEX IF NOT EXISTS idx_book_collections_collection ON book_collections(collection_id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_collections_parent_name
  ON collections(COALESCE(parent_id, 0), name COLLATE NOCASE);
PRAGMA user_version = 3;
COMMIT;
)sql";
    return Exec(store, sql);
}

static int SchemaVersion(LibraryStore* store) {
    sqlite3_stmt* stmt = Prepare(store, "PRAGMA user_version");
    if (!stmt) {
        return -1;
    }
    int version = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return version;
}

LibraryStore* LibraryStoreOpen(Str dbPath) {
    auto* store = new LibraryStore();
    TempStr normalized = NormalizePathTemp(dbPath);
    if (!dir::CreateForFile(normalized)) {
        str::ReplaceWithCopy(&store->error, StrL("cannot create the database directory"));
        return store;
    }
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(CStrTemp(normalized), &store->db, flags, nullptr) != SQLITE_OK) {
        SetError(store, StrL("open"));
        if (store->db) {
            sqlite3_close(store->db);
            store->db = nullptr;
        }
        return store;
    }
    sqlite3_busy_timeout(store->db, 2000);
    if (!Exec(store, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=NORMAL;")) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    int version = SchemaVersion(store);
    if (version < 0 || version > 3) {
        str::ReplaceWithCopy(&store->error, fmt("unsupported library database version: %d", version));
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    if (version < 3) {
        logf("LibraryStore migrating schema: v%d -> v3\n", version);
    }
    if (!CreateSchema(store)) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    logf("LibraryStore opened: '%s' (sqlite %s)\n", normalized, Str(sqlite3_libversion()));
    return store;
}

void LibraryStoreClose(LibraryStore* store) {
    if (!store) {
        return;
    }
    if (store->db) {
        sqlite3_close(store->db);
    }
    str::Free(store->error);
    delete store;
}

bool LibraryStoreIsOpen(LibraryStore* store) {
    return store && store->db;
}

Str LibraryStoreError(LibraryStore* store) {
    return store ? store->error : Str();
}

LibraryBook* LibraryStoreRecordOpen(LibraryStore* store, Str path, Str title, i64 nowMs, bool* placedAtRoot) {
    if (placedAtRoot) {
        *placedAtRoot = false;
    }
    if (!LibraryStoreIsOpen(store) || !path) {
        return nullptr;
    }
    TempStr normalized = NormalizePathTemp(path);
    Str key = PathKey(normalized);
    TempStr fallbackTitle = path::GetBaseNameTemp(normalized);
    if (!title) {
        title = fallbackTitle;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) {
        str::Free(key);
        return nullptr;
    }
    sqlite3_stmt* rootCheck = Prepare(store, R"sql(
SELECT NOT EXISTS(SELECT 1 FROM manual_books m WHERE m.book_id=b.id)
   AND NOT EXISTS(SELECT 1 FROM book_collections bc WHERE bc.book_id=b.id)
FROM books b WHERE b.path_key=?1;
)sql");
    if (!rootCheck) {
        Exec(store, "ROLLBACK");
        str::Free(key);
        return nullptr;
    }
    BindText(rootCheck, 1, key);
    int rootStep = sqlite3_step(rootCheck);
    if (rootStep != SQLITE_ROW && rootStep != SQLITE_DONE) {
        SetError(store, StrL("check opened book placement"));
        sqlite3_finalize(rootCheck);
        Exec(store, "ROLLBACK");
        str::Free(key);
        return nullptr;
    }
    // A new book belongs at the Library root. An existing unplaced book is
    // repaired there, while an organized book keeps its current location.
    bool addToRoot = rootStep == SQLITE_DONE || sqlite3_column_int(rootCheck, 0) != 0;
    sqlite3_finalize(rootCheck);
    const char* sql = R"sql(
INSERT INTO books(path, path_key, title, open_count, reading_seconds, last_read_ms, created_ms, updated_ms)
VALUES(?1, ?2, ?3, 1, 0, ?4, ?4, ?4)
ON CONFLICT(path_key) DO UPDATE SET
  path=excluded.path, title=excluded.title, open_count=books.open_count+1,
  last_read_ms=excluded.last_read_ms, updated_ms=excluded.updated_ms
RETURNING id, path, title, open_count, reading_seconds, last_read_ms;
)sql";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) {
        str::Free(key);
        return nullptr;
    }
    BindText(stmt, 1, normalized);
    BindText(stmt, 2, key);
    BindText(stmt, 3, title);
    sqlite3_bind_int64(stmt, 4, nowMs);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    } else {
        SetError(store, StrL("record open"));
    }
    sqlite3_finalize(stmt);
    if (book && addToRoot) {
        stmt = Prepare(store, "INSERT OR IGNORE INTO manual_books(book_id,added_ms) VALUES(?1,?2)");
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, book->id);
            sqlite3_bind_int64(stmt, 2, nowMs);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                SetError(store, StrL("place opened book at library root"));
                DeleteLibraryBook(book);
                book = nullptr;
            } else if (placedAtRoot) {
                *placedAtRoot = sqlite3_changes(store->db) > 0;
            }
            sqlite3_finalize(stmt);
        } else {
            DeleteLibraryBook(book);
            book = nullptr;
        }
    }
    if (!book || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        DeleteLibraryBook(book);
        book = nullptr;
    }
    str::Free(key);
    return book;
}

LibraryBook* LibraryStoreAddBook(LibraryStore* store, Str path, Str title, i64 nowMs) {
    if (!LibraryStoreIsOpen(store) || !path) {
        return nullptr;
    }
    TempStr normalized = NormalizePathTemp(path);
    Str key = PathKey(normalized);
    TempStr fallbackTitle = path::GetBaseNameTemp(normalized);
    if (!title) {
        title = fallbackTitle;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) {
        str::Free(key);
        return nullptr;
    }
    const char* sql = R"sql(
INSERT INTO books(path, path_key, title, open_count, reading_seconds, last_read_ms, created_ms, updated_ms)
VALUES(?1, ?2, ?3, 0, 0, 0, ?4, ?4)
ON CONFLICT(path_key) DO UPDATE SET path=excluded.path,title=excluded.title,updated_ms=excluded.updated_ms
RETURNING id, path, title, open_count, reading_seconds, last_read_ms;
)sql";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) {
        Exec(store, "ROLLBACK");
        str::Free(key);
        return nullptr;
    }
    BindText(stmt, 1, normalized);
    BindText(stmt, 2, key);
    BindText(stmt, 3, title);
    sqlite3_bind_int64(stmt, 4, nowMs);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    } else {
        SetError(store, StrL("add book"));
    }
    sqlite3_finalize(stmt);
    if (book) {
        stmt = Prepare(store, "INSERT OR IGNORE INTO manual_books(book_id,added_ms) VALUES(?1,?2)");
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, book->id);
            sqlite3_bind_int64(stmt, 2, nowMs);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                SetError(store, StrL("mark manual book"));
                DeleteLibraryBook(book);
                book = nullptr;
            }
            sqlite3_finalize(stmt);
        } else {
            DeleteLibraryBook(book);
            book = nullptr;
        }
    }
    if (!book || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        DeleteLibraryBook(book);
        book = nullptr;
    }
    str::Free(key);
    return book;
}

LibraryBook* LibraryStoreImportBook(LibraryStore* store, Str path, Str title, i64 openCount, i64 nowMs) {
    if (!LibraryStoreIsOpen(store) || !path) {
        return nullptr;
    }
    TempStr normalized = NormalizePathTemp(path);
    Str key = PathKey(normalized);
    TempStr fallbackTitle = path::GetBaseNameTemp(normalized);
    if (!title) {
        title = fallbackTitle;
    }
    openCount = std::max<i64>(openCount, 0);
    const char* sql = R"sql(
INSERT INTO books(path, path_key, title, open_count, reading_seconds, last_read_ms, created_ms, updated_ms)
VALUES(?1, ?2, ?3, ?4, 0, 0, ?5, ?5)
ON CONFLICT(path_key) DO UPDATE SET
  path=excluded.path, open_count=MAX(books.open_count, excluded.open_count), updated_ms=excluded.updated_ms
RETURNING id, path, title, open_count, reading_seconds, last_read_ms;
)sql";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) {
        str::Free(key);
        return nullptr;
    }
    BindText(stmt, 1, normalized);
    BindText(stmt, 2, key);
    BindText(stmt, 3, title);
    sqlite3_bind_int64(stmt, 4, openCount);
    sqlite3_bind_int64(stmt, 5, nowMs);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    } else {
        SetError(store, StrL("import book"));
    }
    sqlite3_finalize(stmt);
    str::Free(key);
    return book;
}

bool LibraryStoreAddReadingTime(LibraryStore* store, Str path, i64 seconds, i64 nowMs) {
    if (!LibraryStoreIsOpen(store) || seconds <= 0) {
        return false;
    }
    Str key = PathKey(path);
    sqlite3_stmt* stmt = Prepare(
        store, "UPDATE books SET reading_seconds=reading_seconds+?1,last_read_ms=?2,updated_ms=?2 WHERE path_key=?3");
    if (!stmt) {
        str::Free(key);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, seconds);
    sqlite3_bind_int64(stmt, 2, nowMs);
    BindText(stmt, 3, key);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("add reading time"));
    }
    sqlite3_finalize(stmt);
    str::Free(key);
    return ok;
}

static const char* SortSql(LibrarySort sort) {
    switch (sort) {
        case LibrarySort::ReadingTime:
            return "b.reading_seconds DESC, b.title COLLATE NOCASE, b.path COLLATE NOCASE";
        case LibrarySort::Recent:
            return "b.last_read_ms DESC, b.title COLLATE NOCASE, b.path COLLATE NOCASE";
        case LibrarySort::Title:
            return "b.title COLLATE NOCASE, b.path COLLATE NOCASE";
        default:
            return "b.open_count DESC, b.title COLLATE NOCASE, b.path COLLATE NOCASE";
    }
}

static Str EscapeLikePattern(Str filter) {
    str::Builder escaped;
    for (int i = 0; i < len(filter); i++) {
        char c = filter.s[i];
        if (c == '\\' || c == '%' || c == '_') {
            escaped.AppendChar('\\');
        }
        escaped.AppendChar(c);
    }
    return escaped.TakeStr();
}

Vec<LibraryBook*> LibraryStoreGetBooks(LibraryStore* store, LibraryBookScope scope, i64 collectionId, LibrarySort sort,
                                       Str filter) {
    Vec<LibraryBook*> books;
    if (!LibraryStoreIsOpen(store)) {
        return books;
    }
    str::Builder sql;
    sql.Append("SELECT b.id,b.path,b.title,b.open_count,b.reading_seconds,b.last_read_ms FROM books b ");
    if (scope == LibraryBookScope::Desk) {
        sql.Append("JOIN desk_books d ON d.book_id=b.id ");
    } else if (scope == LibraryBookScope::Collection) {
        sql.Append("JOIN book_collections bc ON bc.book_id=b.id ");
    } else if (scope == LibraryBookScope::ManualRoot) {
        sql.Append("JOIN manual_books m ON m.book_id=b.id ");
    }
    sql.Append("WHERE 1=1 ");
    if (scope == LibraryBookScope::Unclassified) {
        sql.Append("AND NOT EXISTS(SELECT 1 FROM book_collections x WHERE x.book_id=b.id) ");
    } else if (scope == LibraryBookScope::Collection) {
        sql.Append("AND bc.collection_id=?1 ");
    }
    if (filter) {
        sql.Append(scope == LibraryBookScope::Collection
                       ? "AND (b.title LIKE ?2 ESCAPE '\\' OR b.path LIKE ?2 ESCAPE '\\') "
                       : "AND (b.title LIKE ?1 ESCAPE '\\' OR b.path LIKE ?1 ESCAPE '\\') ");
    }
    sql.Append(fmt("ORDER BY %s", Str(SortSql(sort))));
    sqlite3_stmt* stmt = Prepare(store, CStrTemp(ToStr(sql)));
    if (!stmt) {
        return books;
    }
    int bind = 1;
    if (scope == LibraryBookScope::Collection) {
        sqlite3_bind_int64(stmt, bind++, collectionId);
    }
    if (filter) {
        Str escaped = EscapeLikePattern(filter);
        TempStr like = fmt("%%%s%%", escaped);
        BindText(stmt, bind, like);
        str::Free(escaped);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        books.Append(ReadBook(stmt));
    }
    sqlite3_finalize(stmt);
    if (sort == LibrarySort::Title) {
        VecSort(books, [](LibraryBook* const* a, LibraryBook* const* b) -> int {
            int n = str::CmpNatural((*a)->title, (*b)->title);
            if (n != 0) return n;
            n = str::CmpNatural((*a)->path, (*b)->path);
            if (n != 0) return n;
            return ((*a)->id > (*b)->id) - ((*a)->id < (*b)->id);
        });
    }
    return books;
}

Vec<LibraryCollection*> LibraryStoreGetCollections(LibraryStore* store) {
    Vec<LibraryCollection*> collections;
    if (!LibraryStoreIsOpen(store)) {
        return collections;
    }
    sqlite3_stmt* stmt =
        Prepare(store, "SELECT id,COALESCE(parent_id,0),kind,name FROM collections ORDER BY name COLLATE NOCASE,id");
    if (!stmt) {
        return collections;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* collection = new LibraryCollection();
        collection->id = sqlite3_column_int64(stmt, 0);
        collection->parentId = sqlite3_column_int64(stmt, 1);
        collection->isShelf = sqlite3_column_int(stmt, 2) == 1;
        collection->name = ColumnTextDup(stmt, 3);
        collections.Append(collection);
    }
    sqlite3_finalize(stmt);
    VecSort(collections, [](LibraryCollection* const* a, LibraryCollection* const* b) -> int {
        int n = str::CmpNatural((*a)->name, (*b)->name);
        if (n != 0) return n;
        return ((*a)->id > (*b)->id) - ((*a)->id < (*b)->id);
    });
    return collections;
}

static int CollectionKind(LibraryStore* store, i64 id) {
    if (!store || id <= 0) {
        return 0;
    }
    sqlite3_stmt* stmt = Prepare(store, "SELECT kind FROM collections WHERE id=?1");
    if (!stmt) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, id);
    int kind = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        kind = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return kind;
}

LibraryCollection* LibraryStoreCreateCollection(LibraryStore* store, i64 parentId, bool isShelf, Str name) {
    if (!LibraryStoreIsOpen(store) || !name || (isShelf && parentId != 0) || (!isShelf && parentId == 0)) {
        return nullptr;
    }
    if (!isShelf && CollectionKind(store, parentId) != 1) {
        return nullptr;
    }
    sqlite3_stmt* stmt =
        Prepare(store, "INSERT INTO collections(parent_id,kind,name,created_ms) VALUES(?1,?2,?3,?4) RETURNING id");
    if (!stmt) {
        return nullptr;
    }
    if (parentId)
        sqlite3_bind_int64(stmt, 1, parentId);
    else
        sqlite3_bind_null(stmt, 1);
    sqlite3_bind_int(stmt, 2, isShelf ? 1 : 2);
    BindText(stmt, 3, name);
    sqlite3_bind_int64(stmt, 4, UnixTimeMsNow());
    LibraryCollection* result = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = new LibraryCollection();
        result->id = sqlite3_column_int64(stmt, 0);
        result->parentId = parentId;
        result->isShelf = isShelf;
        result->name = str::Dup(name);
    } else {
        SetError(store, StrL("create collection"));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool LibraryStoreDeleteCollection(LibraryStore* store, i64 collectionId) {
    if (!LibraryStoreIsOpen(store) || collectionId <= 0) return false;
    if (!Exec(store, "BEGIN IMMEDIATE")) return false;
    sqlite3_stmt* stmt = Prepare(store, R"sql(
WITH RECURSIVE subtree(id) AS (
  SELECT id FROM collections WHERE id=?1
  UNION ALL
  SELECT c.id FROM collections c JOIN subtree s ON c.parent_id=s.id
)
INSERT OR IGNORE INTO manual_books(book_id,added_ms)
SELECT DISTINCT bc.book_id,?2
FROM book_collections bc JOIN subtree s ON s.id=bc.collection_id
WHERE NOT EXISTS(
  SELECT 1 FROM book_collections other
  WHERE other.book_id=bc.book_id AND other.collection_id NOT IN (SELECT id FROM subtree)
);
)sql");
    if (!stmt) {
        Exec(store, "ROLLBACK");
        return false;
    }
    sqlite3_bind_int64(stmt, 1, collectionId);
    sqlite3_bind_int64(stmt, 2, UnixTimeMsNow());
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok) {
        stmt = Prepare(store, "DELETE FROM collections WHERE id=?1");
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, collectionId);
            ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
            sqlite3_finalize(stmt);
        } else {
            ok = false;
        }
    }
    if (!ok || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        return false;
    }
    return ok;
}

bool LibraryStoreRenameCollection(LibraryStore* store, i64 collectionId, Str name) {
    if (!LibraryStoreIsOpen(store) || collectionId <= 0 || !name) return false;
    sqlite3_stmt* stmt = Prepare(store, "UPDATE collections SET name=?1 WHERE id=?2");
    if (!stmt) return false;
    BindText(stmt, 1, name);
    sqlite3_bind_int64(stmt, 2, collectionId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) SetError(store, StrL("rename collection"));
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreMoveCollection(LibraryStore* store, i64 collectionId, i64 newParentId) {
    if (!LibraryStoreIsOpen(store) || collectionId <= 0 || collectionId == newParentId) {
        return false;
    }
    int sourceKind = CollectionKind(store, collectionId);
    if (newParentId == 0) {
        if (sourceKind != 1) {
            return false;
        }
    } else if (sourceKind != 2 || CollectionKind(store, newParentId) != 1) {
        return false;
    }
    sqlite3_stmt* check = Prepare(store, R"sql(
WITH RECURSIVE descendants(id) AS (
  SELECT id FROM collections WHERE id=?1
  UNION ALL
  SELECT c.id FROM collections c JOIN descendants d ON c.parent_id=d.id
)
SELECT 1 FROM descendants WHERE id=?2;
)sql");
    if (!check) {
        return false;
    }
    sqlite3_bind_int64(check, 1, collectionId);
    sqlite3_bind_int64(check, 2, newParentId);
    bool cycle = sqlite3_step(check) == SQLITE_ROW;
    sqlite3_finalize(check);
    if (cycle) {
        str::ReplaceWithCopy(&store->error, StrL("moving the collection would create a cycle"));
        return false;
    }
    sqlite3_stmt* stmt = Prepare(store, R"sql(
UPDATE collections SET parent_id=?1,kind=CASE WHEN ?1 IS NULL THEN 1 ELSE 2 END WHERE id=?2;
)sql");
    if (!stmt) {
        return false;
    }
    if (newParentId > 0) {
        sqlite3_bind_int64(stmt, 1, newParentId);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    sqlite3_bind_int64(stmt, 2, collectionId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("move collection"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreAddBookToCollection(LibraryStore* store, i64 bookId, i64 collectionId) {
    if (!LibraryStoreIsOpen(store)) return false;
    sqlite3_stmt* stmt =
        Prepare(store, "INSERT OR IGNORE INTO book_collections(book_id,collection_id,added_ms) VALUES(?1,?2,?3)");
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, bookId);
    sqlite3_bind_int64(stmt, 2, collectionId);
    sqlite3_bind_int64(stmt, 3, UnixTimeMsNow());
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) SetError(store, StrL("add book to collection"));
    sqlite3_finalize(stmt);
    return ok;
}

static sqlite3_stmt* PrepareBookMembership(LibraryStore* store, i64 collectionId, bool insert) {
    if (collectionId == 0) {
        return Prepare(store, insert ? "INSERT OR IGNORE INTO manual_books(book_id,added_ms) VALUES(?1,?2)"
                                     : "DELETE FROM manual_books WHERE book_id=?1");
    }
    return Prepare(store,
                   insert ? "INSERT OR IGNORE INTO book_collections(book_id,collection_id,added_ms) VALUES(?1,?2,?3)"
                          : "DELETE FROM book_collections WHERE book_id=?1 AND collection_id=?2");
}

bool LibraryStorePlaceBook(LibraryStore* store, i64 bookId, i64 sourceCollectionId, i64 targetCollectionId, bool copy) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0 || sourceCollectionId < 0 || targetCollectionId < 0 ||
        sourceCollectionId == targetCollectionId) {
        return false;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) return false;

    sqlite3_stmt* source = Prepare(store, sourceCollectionId == 0
                                              ? "SELECT 1 FROM manual_books WHERE book_id=?1"
                                              : "SELECT 1 FROM book_collections WHERE book_id=?1 AND collection_id=?2");
    if (!source) {
        Exec(store, "ROLLBACK");
        return false;
    }
    sqlite3_bind_int64(source, 1, bookId);
    if (sourceCollectionId > 0) sqlite3_bind_int64(source, 2, sourceCollectionId);
    bool sourceExists = sqlite3_step(source) == SQLITE_ROW;
    sqlite3_finalize(source);
    if (!sourceExists) {
        Exec(store, "ROLLBACK");
        return false;
    }

    sqlite3_stmt* target = PrepareBookMembership(store, targetCollectionId, true);
    bool ok = target != nullptr;
    if (target) {
        sqlite3_bind_int64(target, 1, bookId);
        if (targetCollectionId == 0) {
            sqlite3_bind_int64(target, 2, UnixTimeMsNow());
        } else {
            sqlite3_bind_int64(target, 2, targetCollectionId);
            sqlite3_bind_int64(target, 3, UnixTimeMsNow());
        }
        ok = sqlite3_step(target) == SQLITE_DONE;
        if (!ok) SetError(store, StrL("copy book membership"));
        sqlite3_finalize(target);
    }
    if (ok && !copy) {
        source = PrepareBookMembership(store, sourceCollectionId, false);
        ok = source != nullptr;
        if (source) {
            sqlite3_bind_int64(source, 1, bookId);
            if (sourceCollectionId > 0) sqlite3_bind_int64(source, 2, sourceCollectionId);
            ok = sqlite3_step(source) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
            if (!ok) SetError(store, StrL("move book membership"));
            sqlite3_finalize(source);
        }
    }
    if (!ok || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        return false;
    }
    return true;
}

bool LibraryStoreSetBookOnDesk(LibraryStore* store, i64 bookId, bool onDesk) {
    if (!LibraryStoreIsOpen(store)) return false;
    const char* sql = onDesk ? "INSERT OR IGNORE INTO desk_books(book_id,added_ms) VALUES(?1,?2)"
                             : "DELETE FROM desk_books WHERE book_id=?1";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, bookId);
    if (onDesk) sqlite3_bind_int64(stmt, 2, UnixTimeMsNow());
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) SetError(store, StrL("update desk"));
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreRemoveBook(LibraryStore* store, i64 bookId) {
    if (!LibraryStoreIsOpen(store)) return false;
    sqlite3_stmt* stmt = Prepare(store, "DELETE FROM books WHERE id=?1");
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, bookId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    sqlite3_finalize(stmt);
    return ok;
}

static bool HasPrefixBoundary(Str path, Str prefix) {
    if (!str::StartsWithI(path, prefix)) return false;
    if (len(path) == len(prefix) || path.s[len(prefix) - 1] == '\\' || path.s[len(prefix) - 1] == '/') return true;
    char c = path.s[len(prefix)];
    return c == '\\' || c == '/';
}

Vec<LibraryPathChange*> LibraryStorePreviewPathReplace(LibraryStore* store, Str oldPrefix, Str newPrefix) {
    Vec<LibraryPathChange*> changes;
    if (!LibraryStoreIsOpen(store)) return changes;
    TempStr oldNorm = NormalizePathTemp(oldPrefix);
    TempStr newNorm = NormalizePathTemp(newPrefix);
    Vec<LibraryBook*> books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Title, Str());
    for (LibraryBook* book : books) {
        if (!HasPrefixBoundary(book->path, oldNorm)) continue;
        Str tail(book->path.s + len(oldNorm), len(book->path) - len(oldNorm));
        TempStr replaced = str::JoinTemp(newNorm, tail);
        auto* change = new LibraryPathChange();
        change->bookId = book->id;
        change->oldPath = str::Dup(book->path);
        change->newPath = str::Dup(NormalizePathTemp(replaced));
        change->targetExists = file::Exists(change->newPath);
        Str key = PathKey(change->newPath);
        sqlite3_stmt* stmt = Prepare(store, "SELECT 1 FROM books WHERE path_key=?1 AND id<>?2");
        if (stmt) {
            BindText(stmt, 1, key);
            sqlite3_bind_int64(stmt, 2, book->id);
            change->conflict = sqlite3_step(stmt) == SQLITE_ROW;
            sqlite3_finalize(stmt);
        }
        str::Free(key);
        changes.Append(change);
    }
    DeleteLibraryBooks(books);
    return changes;
}

bool LibraryStoreApplyPathReplace(LibraryStore* store, Vec<LibraryPathChange*>& changes) {
    if (!LibraryStoreIsOpen(store) || len(changes) == 0) return false;
    for (LibraryPathChange* change : changes) {
        if (change->conflict) return false;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) return false;
    sqlite3_stmt* stmt = Prepare(store, "UPDATE books SET path=?1,path_key=?2,updated_ms=?3 WHERE id=?4 AND path=?5");
    bool ok = stmt != nullptr;
    for (LibraryPathChange* change : changes) {
        if (!ok) break;
        Str key = PathKey(change->newPath);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        BindText(stmt, 1, change->newPath);
        BindText(stmt, 2, key);
        sqlite3_bind_int64(stmt, 3, UnixTimeMsNow());
        sqlite3_bind_int64(stmt, 4, change->bookId);
        BindText(stmt, 5, change->oldPath);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
        str::Free(key);
    }
    sqlite3_finalize(stmt);
    if (ok)
        ok = Exec(store, "COMMIT");
    else
        Exec(store, "ROLLBACK");
    return ok;
}

#if defined(DEBUG)
#include "base/UtAssert.h"

static void AssertBookPaths(Vec<LibraryBook*>& books, const char* first, const char* second, const char* third) {
    utassert(len(books) == 3);
    utassert(str::EqI(books[0]->path, Str(first)));
    utassert(str::EqI(books[1]->path, Str(second)));
    utassert(str::EqI(books[2]->path, Str(third)));
    DeleteLibraryBooks(books);
}

static void TestLibraryStableSorts() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    TempStr dbPath = path::JoinTemp(dir, fmt("sort-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));

    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\B.pdf"), StrL("Same"), 100));
    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\A.pdf"), StrL("Same"), 200));
    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\C.pdf"), StrL("Zed"), 300));
    DeleteLibraryBook(LibraryStoreRecordOpen(store, StrL("C:\\Sort\\B.pdf"), StrL("Same"), 1000));
    DeleteLibraryBook(LibraryStoreRecordOpen(store, StrL("C:\\Sort\\C.pdf"), StrL("Zed"), 2000));
    DeleteLibraryBook(LibraryStoreRecordOpen(store, StrL("C:\\Sort\\C.pdf"), StrL("Zed"), 3000));
    utassert(LibraryStoreAddReadingTime(store, StrL("C:\\Sort\\A.pdf"), 30, 4000));
    utassert(LibraryStoreAddReadingTime(store, StrL("C:\\Sort\\B.pdf"), 10, 5000));
    utassert(LibraryStoreAddReadingTime(store, StrL("C:\\Sort\\C.pdf"), 20, 6000));

    Vec<LibraryBook*> books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::OpenCount, Str());
    AssertBookPaths(books, "C:\\Sort\\C.pdf", "C:\\Sort\\B.pdf", "C:\\Sort\\A.pdf");
    books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::ReadingTime, Str());
    AssertBookPaths(books, "C:\\Sort\\A.pdf", "C:\\Sort\\C.pdf", "C:\\Sort\\B.pdf");
    books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Recent, Str());
    AssertBookPaths(books, "C:\\Sort\\C.pdf", "C:\\Sort\\B.pdf", "C:\\Sort\\A.pdf");
    books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Title, Str());
    AssertBookPaths(books, "C:\\Sort\\A.pdf", "C:\\Sort\\B.pdf", "C:\\Sort\\C.pdf");

    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
}

static void TestLibraryNaturalTitleSort() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    TempStr dbPath = path::JoinTemp(dir, fmt("natural-sort-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));

    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\A.pdf"), StrL("Book 10"), 100));
    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\B.pdf"), StrL("Book 2"), 100));
    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\C.pdf"), StrL("Book 1"), 100));
    Vec<LibraryBook*> books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Title, Str());
    AssertBookPaths(books, "C:\\Sort\\C.pdf", "C:\\Sort\\B.pdf", "C:\\Sort\\A.pdf");

    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
}

static void TestLibraryPathReplaceAndPersistence() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    TempStr dbPath = path::JoinTemp(dir, fmt("paths-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));

    LibraryBook* a = LibraryStoreAddBook(store, StrL("C:\\Old\\A.pdf"), StrL("A"), 100);
    LibraryBook* b = LibraryStoreAddBook(store, StrL("C:\\Old\\B.pdf"), StrL("B"), 200);
    LibraryBook* existing = LibraryStoreAddBook(store, StrL("D:\\Dest\\A.pdf"), StrL("Existing"), 300);
    LibraryBook* chinese = LibraryStoreAddBook(store, StrL("C:\\旧书\\中文.pdf"), StrL("中文"), 400);
    utassert(a && b && existing && chinese);

    Vec<LibraryPathChange*> changes = LibraryStorePreviewPathReplace(store, StrL("C:\\Ol"), StrL("D:\\Wrong"));
    utassert(len(changes) == 0);
    DeleteLibraryPathChanges(changes);
    changes = LibraryStorePreviewPathReplace(store, StrL("c:\\OLD"), StrL("D:\\Dest"));
    utassert(len(changes) == 2 && changes[0]->conflict && !changes[1]->conflict);
    utassert(!LibraryStoreApplyPathReplace(store, changes));
    DeleteLibraryPathChanges(changes);

    changes = LibraryStorePreviewPathReplace(store, StrL("C:\\Old"), StrL("E:\\Moved"));
    utassert(len(changes) == 2 && !changes[0]->conflict && !changes[1]->conflict);
    str::ReplaceWithCopy(&changes[1]->oldPath, StrL("C:\\Old\\Missing.pdf"));
    utassert(!LibraryStoreApplyPathReplace(store, changes));
    DeleteLibraryPathChanges(changes);
    changes = LibraryStorePreviewPathReplace(store, StrL("C:\\Old"), StrL("E:\\Moved"));
    utassert(len(changes) == 2);
    DeleteLibraryPathChanges(changes);

    changes = LibraryStorePreviewPathReplace(store, StrL("c:\\旧书"), StrL("D:\\新书"));
    utassert(len(changes) == 1 && str::EqI(changes[0]->newPath, StrL("D:\\新书\\中文.pdf")));
    DeleteLibraryPathChanges(changes);

    LibraryCollection* shelf = LibraryStoreCreateCollection(store, 0, true, StrL("Shelf"));
    LibraryCollection* child = LibraryStoreCreateCollection(store, shelf->id, false, StrL("Child"));
    utassert(shelf && child);
    utassert(LibraryStoreRenameCollection(store, child->id, StrL("Renamed")));
    utassert(!LibraryStoreRenameCollection(store, child->id, StrL("")));
    utassert(LibraryStoreAddBookToCollection(store, a->id, child->id));
    utassert(LibraryStoreSetBookOnDesk(store, a->id, true));
    utassert(LibraryStoreDeleteCollection(store, shelf->id));
    Vec<LibraryCollection*> collections = LibraryStoreGetCollections(store);
    utassert(len(collections) == 0);
    DeleteLibraryCollections(collections);
    Vec<LibraryBook*> unclassified =
        LibraryStoreGetBooks(store, LibraryBookScope::Unclassified, 0, LibrarySort::Title, Str());
    utassert(len(unclassified) == 4);
    DeleteLibraryBooks(unclassified);

    DeleteLibraryCollection(shelf);
    DeleteLibraryCollection(child);
    DeleteLibraryBook(a);
    DeleteLibraryBook(b);
    DeleteLibraryBook(existing);
    DeleteLibraryBook(chinese);
    LibraryStoreClose(store);
    store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));
    Vec<LibraryBook*> desk = LibraryStoreGetBooks(store, LibraryBookScope::Desk, 0, LibrarySort::Title, Str());
    utassert(len(desk) == 1 && str::EqI(desk[0]->path, StrL("C:\\Old\\A.pdf")));
    DeleteLibraryBooks(desk);
    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
}

void LibraryStore_UnitTests() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    dir::CreateAll(dir);
    TempStr dbPath = path::JoinTemp(dir, fmt("library-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));
    LibraryBook* a = LibraryStoreRecordOpen(store, StrL("C:\\Books\\Alpha.pdf"), StrL("Alpha"), 1000);
    LibraryBook* b = LibraryStoreRecordOpen(store, StrL("C:\\Books\\Beta.pdf"), StrL("Beta"), 2000);
    utassert(a && b && a->id != b->id);
    DeleteLibraryBook(LibraryStoreRecordOpen(store, StrL("c:\\books\\alpha.pdf"), StrL("Alpha"), 3000));
    LibraryBook* imported = LibraryStoreAddBook(store, StrL("C:\\Books\\100%_Guide.pdf"), StrL("100%_Guide"), 3500);
    utassert(imported && imported->openCount == 0);
    DeleteLibraryBook(LibraryStoreAddBook(store, imported->path, imported->title, 3600));
    LibraryBook* legacy = LibraryStoreImportBook(store, StrL("C:\\Books\\Legacy.pdf"), StrL("Legacy"), 12, 3700);
    utassert(legacy && legacy->openCount == 12);
    DeleteLibraryBook(legacy);
    legacy = LibraryStoreImportBook(store, StrL("c:\\books\\legacy.pdf"), StrL("Legacy"), 12, 3800);
    utassert(legacy && legacy->openCount == 12);
    DeleteLibraryBook(legacy);
    legacy = LibraryStoreImportBook(store, StrL("C:\\Books\\Legacy.pdf"), StrL("Legacy"), 8, 3900);
    utassert(legacy && legacy->openCount == 12);
    DeleteLibraryBook(legacy);
    Vec<LibraryBook*> manualRoot =
        LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 3 && manualRoot[0]->id == imported->id && manualRoot[1]->id == a->id &&
             manualRoot[2]->id == b->id);
    DeleteLibraryBooks(manualRoot);
    utassert(LibraryStoreAddReadingTime(store, a->path, 90, 4000));
    LibraryCollection* shelf = LibraryStoreCreateCollection(store, 0, true, StrL("Work"));
    LibraryCollection* category = LibraryStoreCreateCollection(store, shelf->id, false, StrL("Architecture"));
    utassert(shelf && category);
    // Normal drag moves from the root. Reopening an organized book updates its
    // statistics without putting it back at the root.
    utassert(LibraryStorePlaceBook(store, a->id, 0, category->id, false));
    DeleteLibraryBook(LibraryStoreRecordOpen(store, a->path, a->title, 4100));
    manualRoot = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 2 && manualRoot[0]->id == imported->id && manualRoot[1]->id == b->id);
    DeleteLibraryBooks(manualRoot);
    utassert(!LibraryStoreCreateCollection(store, category->id, false, StrL("Nested")));
    LibraryCollection* other = LibraryStoreCreateCollection(store, shelf->id, false, StrL("Other"));
    utassert(other);
    // Ctrl-drag copies. A later normal drag to the same destination removes
    // only the source membership and keeps the existing destination.
    utassert(LibraryStorePlaceBook(store, a->id, category->id, other->id, true));
    utassert(LibraryStorePlaceBook(store, a->id, category->id, other->id, false));
    Vec<LibraryBook*> categoryBooks =
        LibraryStoreGetBooks(store, LibraryBookScope::Collection, category->id, LibrarySort::Title, Str());
    Vec<LibraryBook*> otherBooks =
        LibraryStoreGetBooks(store, LibraryBookScope::Collection, other->id, LibrarySort::Title, Str());
    utassert(len(categoryBooks) == 0 && len(otherBooks) == 1 && otherBooks[0]->id == a->id);
    DeleteLibraryBooks(categoryBooks);
    DeleteLibraryBooks(otherBooks);
    // Copy and then move a root book proves that root is a real membership,
    // not a derived "unclassified" view.
    utassert(LibraryStorePlaceBook(store, b->id, 0, category->id, true));
    manualRoot = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 2);
    DeleteLibraryBooks(manualRoot);
    utassert(LibraryStorePlaceBook(store, b->id, 0, category->id, false));
    manualRoot = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 1 && manualRoot[0]->id == imported->id);
    DeleteLibraryBooks(manualRoot);
    LibraryCollection* shelf2 = LibraryStoreCreateCollection(store, 0, true, StrL("More"));
    utassert(shelf2);
    utassert(!LibraryStoreMoveCollection(store, shelf->id, other->id));
    utassert(!LibraryStoreMoveCollection(store, other->id, category->id));
    utassert(!LibraryStoreMoveCollection(store, category->id, 0));
    utassert(LibraryStoreMoveCollection(store, other->id, shelf2->id));
    utassert(LibraryStoreMoveCollection(store, other->id, shelf->id));
    utassert(LibraryStoreDeleteCollection(store, shelf2->id));
    DeleteLibraryCollection(shelf2);
    utassert(LibraryStoreSetBookOnDesk(store, a->id, true));
    Vec<LibraryBook*> all = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::OpenCount, Str());
    utassert(len(all) == 4 && str::EqI(all[0]->path, StrL("C:\\Books\\Legacy.pdf")));
    DeleteLibraryBooks(all);
    Vec<LibraryBook*> escapedFilter =
        LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Title, StrL("%_"));
    utassert(len(escapedFilter) == 1 && escapedFilter[0]->id == imported->id);
    DeleteLibraryBooks(escapedFilter);
    Vec<LibraryBook*> desk = LibraryStoreGetBooks(store, LibraryBookScope::Desk, 0, LibrarySort::Title, Str());
    utassert(len(desk) == 1 && desk[0]->id == a->id);
    DeleteLibraryBooks(desk);
    Vec<LibraryBook*> unclassified =
        LibraryStoreGetBooks(store, LibraryBookScope::Unclassified, 0, LibrarySort::Title, Str());
    utassert(len(unclassified) == 2 && unclassified[0]->id == imported->id &&
             str::EqI(unclassified[1]->path, StrL("C:\\Books\\Legacy.pdf")));
    DeleteLibraryBooks(unclassified);
    Vec<LibraryPathChange*> changes = LibraryStorePreviewPathReplace(store, StrL("C:\\Books"), StrL("D:\\Moved Books"));
    utassert(len(changes) == 4);
    utassert(LibraryStoreApplyPathReplace(store, changes));
    DeleteLibraryPathChanges(changes);
    utassert(LibraryStoreDeleteCollection(store, shelf->id));
    manualRoot = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 3 && manualRoot[0]->id == imported->id && manualRoot[1]->id == a->id &&
             manualRoot[2]->id == b->id);
    DeleteLibraryBooks(manualRoot);
    DeleteLibraryCollection(category);
    DeleteLibraryCollection(other);
    DeleteLibraryCollection(shelf);
    DeleteLibraryBook(a);
    DeleteLibraryBook(b);
    DeleteLibraryBook(imported);
    LibraryStoreClose(store);
    sqlite3* rawDb = nullptr;
    utassert(sqlite3_open(CStrTemp(dbPath), &rawDb) == SQLITE_OK);
    utassert(sqlite3_exec(rawDb, "PRAGMA user_version=1", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(rawDb);
    store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));
    LibraryStoreClose(store);
    utassert(sqlite3_open(CStrTemp(dbPath), &rawDb) == SQLITE_OK);
    utassert(sqlite3_exec(rawDb, "PRAGMA user_version=99", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(rawDb);
    store = LibraryStoreOpen(dbPath);
    utassert(!LibraryStoreIsOpen(store));
    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
    TestLibraryStableSorts();
    TestLibraryNaturalTitleSort();
    TestLibraryPathReplaceAndPersistence();
}
#endif
