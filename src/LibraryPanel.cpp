/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/DirScan.h"
#include "base/File.h"
#include "base/Win.h"

#include "gui/Dpi.h"
#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "FileHistory.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Tabs.h"
#include "Library.h"
#include "LibraryStore.h"
#include "SvgIcons.h"
#include "Theme.h"
#include "Translations.h"

#include "LibraryPanel.h"
#include "SumatraLog.h"

enum class LibraryTreeKind {
    Root,
    Collection,
    Book,
    Error,
};

struct LibraryTreeItem {
    ~LibraryTreeItem();

    uintptr_t userData = 0;
    LibraryTreeItem* parent = nullptr;
    Vec<LibraryTreeItem*> children;
    LibraryTreeKind kind = LibraryTreeKind::Root;
    Str text;
    Str path;
    i64 bookId = 0;
    i64 collectionId = 0;
    bool expanded = false;
};

LibraryTreeItem::~LibraryTreeItem() {
    DeleteVecMembers(children);
    str::Free(text);
    str::Free(path);
}

struct LibraryTreeModel : TreeModel {
    ~LibraryTreeModel() override { delete root; }
    TreeItem Root() override { return (TreeItem)root; }
    Str Text(TreeItem item) override { return ((LibraryTreeItem*)item)->text; }
    TreeItem Parent(TreeItem item) override { return (TreeItem)((LibraryTreeItem*)item)->parent; }
    int ChildCount(TreeItem item) override { return len(((LibraryTreeItem*)item)->children); }
    TreeItem ChildAt(TreeItem item, int idx) override { return (TreeItem)((LibraryTreeItem*)item)->children[idx]; }
    bool IsExpanded(TreeItem item) override { return ((LibraryTreeItem*)item)->expanded; }
    bool IsChecked(TreeItem) override { return false; }
    void SetUserData(TreeItem item, uintptr_t data) override { ((LibraryTreeItem*)item)->userData = data; }
    uintptr_t GetUserData(TreeItem item) override { return ((LibraryTreeItem*)item)->userData; }

    LibraryTreeItem* root = nullptr;
};

static LibraryTreeItem* NewItem(LibraryTreeItem* parent, LibraryTreeKind kind, Str text) {
    auto* item = new LibraryTreeItem();
    item->parent = parent;
    item->kind = kind;
    item->text = str::Dup(text);
    if (parent) parent->children.Append(item);
    return item;
}

static void AddBooks(LibraryTreeItem* parent, LibraryBookScope scope, i64 collectionId, Str filter) {
    Vec<LibraryBook*> books = LibraryStoreGetBooks(LibraryGetStore(), scope, collectionId, LibrarySort::Title, filter);
    for (LibraryBook* book : books) {
        auto* item = NewItem(parent, LibraryTreeKind::Book, book->title);
        item->bookId = book->id;
        item->path = str::Dup(book->path);
    }
    DeleteLibraryBooks(books);
}

static void PruneEmptyCollections(LibraryTreeItem* parent) {
    for (int i = len(parent->children) - 1; i >= 0; i--) {
        LibraryTreeItem* child = parent->children[i];
        if (child->kind != LibraryTreeKind::Collection) continue;
        PruneEmptyCollections(child);
        if (len(child->children) == 0) {
            parent->children.RemoveAt(i);
            delete child;
        }
    }
}

static bool IsPdfTab(WindowTab* tab) {
    return tab && tab->filePath && str::EndsWithI(tab->filePath, StrL(".pdf"));
}

static LibraryTreeModel* BuildModel(MainWindow* win, Str filter) {
    auto* model = new LibraryTreeModel();
    model->root = NewItem(nullptr, LibraryTreeKind::Root, Str());
    if (!LibraryIsAvailable()) {
        TempStr msg = fmt("Library unavailable: %s", LibraryGetError());
        NewItem(model->root, LibraryTreeKind::Error, msg);
        return model;
    }

    AddBooks(model->root, LibraryBookScope::ManualRoot, 0, filter);

    Vec<LibraryCollection*> collections = LibraryStoreGetCollections(LibraryGetStore());
    Vec<LibraryTreeItem*> collectionItems;
    for (LibraryCollection* collection : collections) {
        auto* item = new LibraryTreeItem();
        item->kind = LibraryTreeKind::Collection;
        item->text = str::Dup(collection->name);
        item->collectionId = collection->id;
        item->expanded =
            filter ? true
                   : (!win->libraryExpansionInitialized ? collection->isShelf
                                                        : win->expandedLibraryCollections.Contains(collection->id));
        collectionItems.Append(item);
    }
    for (int i = 0; i < len(collections); i++) {
        LibraryCollection* collection = collections[i];
        LibraryTreeItem* item = collectionItems[i];
        LibraryTreeItem* parent = model->root;
        if (collection->parentId) {
            for (int j = 0; j < len(collections); j++) {
                if (collections[j]->id == collection->parentId) {
                    parent = collectionItems[j];
                    break;
                }
            }
        }
        item->parent = parent;
        parent->children.Append(item);
        AddBooks(item, LibraryBookScope::Collection, collection->id, filter);
    }
    DeleteLibraryCollections(collections);
    if (filter) {
        PruneEmptyCollections(model->root);
    }
    return model;
}

static TempStr FilterTextTemp(MainWindow* win) {
    return win && win->libraryFilterEdit ? win->libraryFilterEdit->GetTextTemp() : TempStr();
}

static void RememberExpandedCollectionsRec(MainWindow* win, LibraryTreeItem* item) {
    if (item->kind == LibraryTreeKind::Collection && win->libraryTreeView->IsExpanded((TreeItem)item)) {
        win->expandedLibraryCollections.Append(item->collectionId);
    }
    for (LibraryTreeItem* child : item->children) {
        RememberExpandedCollectionsRec(win, child);
    }
}

static void RememberExpandedCollections(MainWindow* win) {
    if (!win || !win->libraryTreeView || !win->libraryTreeView->treeModel || win->libraryModelFiltered) return;
    win->expandedLibraryCollections.Reset();
    auto* model = (LibraryTreeModel*)win->libraryTreeView->treeModel;
    RememberExpandedCollectionsRec(win, model->root);
    win->libraryExpansionInitialized = true;
}

void RefreshLibraryPanel(MainWindow* win) {
    if (!win || !win->libraryTreeView) return;
    if (win->libraryDragging) ReleaseCapture();
    win->libraryDragItem = 0;
    win->libraryDragging = false;
    RememberExpandedCollections(win);
    TempStr filter = FilterTextTemp(win);
    TreeModel* previous = win->libraryTreeView->treeModel;
    win->libraryTreeView->SetTreeModel(BuildModel(win, filter));
    win->libraryModelFiltered = !!filter;
    win->libraryExpansionInitialized = true;
    delete previous;
}

void RefreshLibraryPanels() {
    for (MainWindow* win : gWindows) {
        RefreshLibraryPanel(win);
    }
}

static void OnFilterChanged(MainWindow* win) {
    RefreshLibraryPanel(win);
}

static void OpenLibraryItem(MainWindow* source, LibraryTreeItem* item) {
    if (!item || !item->path) return;
    MainWindow* existing = FindMainWindowByFile(item->path, true);
    if (existing) {
        existing->Focus();
        return;
    }
    LoadArgs args(item->path, source);
    StartLoadDocument(&args);
}

static void OnTreeClick(TreeView::ClickEvent* ev) {
    if (!ev->treeItem) return;
    auto* item = (LibraryTreeItem*)ev->treeItem;
    if (item->kind != LibraryTreeKind::Book) return;
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    OpenLibraryItem(win, item);
}

static void OnTreeTooltip(TreeView::GetTooltipEvent* ev) {
    auto* item = (LibraryTreeItem*)ev->treeItem;
    if (item && item->path) {
        str::BufSet(ev->info->pszText, ev->info->cchTextMax, item->path);
    }
}

enum {
    kLibraryMenuRemoveBook = 1,
    kLibraryMenuDeleteCollection,
    kLibraryMenuRenameCollection,
};

static Str PromptLibraryText(HWND parent, Str title, Str label);

static void OnTreeContextMenu(ContextMenuEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    if (!win) return;
    TreeItem selected = win->libraryTreeView->GetItemAt(ev->mouseWindow.x, ev->mouseWindow.y);
    if (!selected) selected = win->libraryTreeView->GetSelection();
    auto* item = (LibraryTreeItem*)selected;
    if (!item) return;
    HMENU menu = CreatePopupMenu();
    if (item->kind == LibraryTreeKind::Book) {
        AppendMenuW(menu, MF_STRING, kLibraryMenuRemoveBook, CWStrTemp(_TRA("Remove from Library")));
    } else if (item->kind == LibraryTreeKind::Collection) {
        AppendMenuW(menu, MF_STRING, kLibraryMenuRenameCollection, CWStrTemp(_TRA("Rename Shelf or Category")));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuDeleteCollection, CWStrTemp(_TRA("Delete Shelf or Category")));
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ev->mouseScreen.x, ev->mouseScreen.y, 0,
                             win->hwndFrame, nullptr);
    DestroyMenu(menu);
    if (cmd == kLibraryMenuRemoveBook) LibraryStoreRemoveBook(LibraryGetStore(), item->bookId);
    if (cmd == kLibraryMenuDeleteCollection) LibraryStoreDeleteCollection(LibraryGetStore(), item->collectionId);
    if (cmd == kLibraryMenuRenameCollection) {
        Str name = PromptLibraryText(win->hwndFrame, _TRA("Rename Shelf or Category"), _TRA("New name"));
        if (name) {
            LibraryStoreRenameCollection(LibraryGetStore(), item->collectionId, name);
            str::Free(name);
        }
    }
    if (cmd) RefreshLibraryPanels();
}

static LRESULT CALLBACK LibraryTreeSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    MainWindow* win = (MainWindow*)data;
    if (msg == WM_LBUTTONDOWN && win && win->libraryTreeView) {
        win->libraryDragItem = win->libraryTreeView->GetItemAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        win->libraryDragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        win->libraryDragging = false;
    }
    if (msg == WM_MOUSEMOVE && win && win->libraryDragItem && (wp & MK_LBUTTON)) {
        int dx = std::abs(GET_X_LPARAM(lp) - win->libraryDragStart.x);
        int dy = std::abs(GET_Y_LPARAM(lp) - win->libraryDragStart.y);
        if (!win->libraryDragging && (dx >= GetSystemMetrics(SM_CXDRAG) || dy >= GetSystemMetrics(SM_CYDRAG))) {
            win->libraryDragging = true;
            SetCapture(hwnd);
        }
        if (win->libraryDragging) return 0;
    }
    if (msg == WM_MOUSEMOVE && win && !win->libraryDragging) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        HwndInvalidate(hwnd);
    }
    if (msg == WM_MOUSELEAVE) {
        HwndInvalidate(hwnd);
    }
    if (msg == WM_LBUTTONUP && win && win->libraryDragging && win->libraryTreeView) {
        auto* source = (LibraryTreeItem*)win->libraryDragItem;
        auto* target = (LibraryTreeItem*)win->libraryTreeView->GetItemAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        bool copy = (wp & MK_CONTROL) != 0;
        bool changed = false;
        if (source && source != target) {
            if (source->kind == LibraryTreeKind::Book) {
                i64 sourceCollectionId = source->parent && source->parent->kind == LibraryTreeKind::Collection
                                             ? source->parent->collectionId
                                             : 0;
                i64 targetCollectionId =
                    target && target->kind == LibraryTreeKind::Collection ? target->collectionId : 0;
                if (!target || target->kind == LibraryTreeKind::Collection) {
                    changed = LibraryStorePlaceBook(LibraryGetStore(), source->bookId, sourceCollectionId,
                                                    targetCollectionId, copy);
                }
            } else if (!copy && source->kind == LibraryTreeKind::Collection && target &&
                       target->kind == LibraryTreeKind::Collection) {
                changed = LibraryStoreMoveCollection(LibraryGetStore(), source->collectionId, target->collectionId);
            } else if (!copy && source->kind == LibraryTreeKind::Collection && !target) {
                changed = LibraryStoreMoveCollection(LibraryGetStore(), source->collectionId, 0);
            }
        }
        ReleaseCapture();
        win->libraryDragItem = 0;
        win->libraryDragging = false;
        if (changed) RefreshLibraryPanels();
        return 0;
    }
    if (msg == WM_CAPTURECHANGED && win) {
        win->libraryDragItem = 0;
        win->libraryDragging = false;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void CloseLibrary(MainWindow* win, VirtMouseEvent*) {
    SetLibraryPanelVisible(win, false);
}

enum {
    kLibraryAddShelf = 1,
    kLibraryAddCategory,
    kLibraryAddPdf,
    kLibraryImportDir,
    kLibraryReplacePath,
};

enum {
    kLibraryPromptEdit = 1001
};

struct LibraryPromptState {
    Str title;
    Str label;
    HWND edit = nullptr;
    Str result;
};

#pragma pack(push, 2)
struct LibraryPromptTemplate {
    DLGTEMPLATE dlg{};
    WORD menu = 0;
    WORD windowClass = 0;
    WCHAR title = 0;
};
#pragma pack(pop)

static INT_PTR CALLBACK LibraryPromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = (LibraryPromptState*)GetWindowLongPtrW(hwnd, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        state = (LibraryPromptState*)lp;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)state);
        SetWindowTextW(hwnd, CWStrTemp(state->title));
        HFONT font = GetAppFont()->GetHFont();
        Rect rc = HwndClientRect(hwnd);
        HWND label = CreateWindowW(WC_STATICW, CWStrTemp(state->label), WS_CHILD | WS_VISIBLE, 12, 12, rc.dx - 24, 20,
                                   hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        state->edit =
            CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12,
                            35, rc.dx - 24, 24, hwnd, (HMENU)kLibraryPromptEdit, GetModuleHandleW(nullptr), nullptr);
        HWND ok =
            CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("OK")), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                          rc.dx - 174, rc.dy - 38, 76, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        HWND cancel =
            CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("Cancel")), WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.dx - 88,
                          rc.dy - 38, 76, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        HWND controls[] = {label, state->edit, ok, cancel};
        for (HWND control : controls) {
            SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
        }
        HwndSetFocus(state->edit);
        return FALSE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK && state) {
        int n = GetWindowTextLengthW(state->edit);
        WCHAR* value = AllocArrayTemp<WCHAR>(n + 1);
        GetWindowTextW(state->edit, value, n + 1);
        state->result = str::Dup(ToUtf8Temp(value));
        str::TrimWSInPlace(state->result, str::TrimOpt::Both);
        if (!state->result) {
            MessageBeep(MB_ICONWARNING);
            return TRUE;
        }
        EndDialog(hwnd, IDOK);
        return TRUE;
    }
    if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE) {
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static Str PromptLibraryText(HWND parent, Str title, Str label) {
    LibraryPromptState state{title, label};
    LibraryPromptTemplate t;
    t.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dlg.dwExtendedStyle = WS_EX_DLGMODALFRAME;
    t.dlg.cx = 280;
    t.dlg.cy = 92;
    INT_PTR result =
        DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &t.dlg, parent, LibraryPromptProc, (LPARAM)&state);
    if (result != IDOK) {
        str::Free(state.result);
        return {};
    }
    return state.result;
}

static i64 SelectedCollectionId(MainWindow* win) {
    TreeItem selected = win->libraryTreeView ? win->libraryTreeView->GetSelection() : 0;
    auto* item = (LibraryTreeItem*)selected;
    if (item && item->kind == LibraryTreeKind::Collection) return item->collectionId;
    if (item && item->kind == LibraryTreeKind::Book && item->parent &&
        item->parent->kind == LibraryTreeKind::Collection) {
        return item->parent->collectionId;
    }
    return 0;
}

static bool AddPdfToLibrary(MainWindow* win, Str filePath) {
    LibraryBook* book =
        LibraryStoreAddBook(LibraryGetStore(), filePath, path::GetBaseNameTemp(filePath), UnixTimeMsNow());
    bool ok = book != nullptr;
    i64 collectionId = SelectedCollectionId(win);
    if (book && collectionId) {
        ok = LibraryStorePlaceBook(LibraryGetStore(), book->id, 0, collectionId, false);
    }
    DeleteLibraryBook(book);
    return ok;
}

static void AddPdfFiles(MainWindow* win) {
    constexpr DWORD cch = 64 * 1024;
    WCHAR* files = AllocArrayTemp<WCHAR>(cch);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = win->hwndFrame;
    WCHAR filter[128]{};
    WCHAR* filterName = CWStrTemp(_TRA("PDF files"));
    int filterNameLen = _snwprintf_s(filter, dimof(filter), _TRUNCATE, L"%s (*.pdf)", filterName);
    if (filterNameLen < 0) return;
    wcscpy_s(filter + filterNameLen + 1, dimof(filter) - filterNameLen - 1, L"*.pdf");
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = files;
    ofn.nMaxFile = cch;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn)) return;

    WCHAR* first = files;
    WCHAR* next = first + wcslen(first) + 1;
    int imported = 0;
    if (!*next) {
        TempStr path = ToUtf8Temp(first);
        imported = AddPdfToLibrary(win, path) ? 1 : 0;
    } else {
        TempStr dir = ToUtf8Temp(first);
        while (*next) {
            TempStr path = path::JoinTemp(dir, ToUtf8Temp(next));
            if (AddPdfToLibrary(win, path)) imported++;
            next += wcslen(next) + 1;
        }
    }
    logf("Library file import: %d PDFs\n", imported);
    RefreshLibraryPanels();
}

static bool IsLibraryPathOpen(Str path) {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            if (IsPdfTab(tab) && str::EqI(path::NormalizeTemp(tab->filePath), path::NormalizeTemp(path))) return true;
        }
    }
    return false;
}

static void ReplaceLibraryPathPrefix(MainWindow* win) {
    Str oldPrefix = PromptLibraryText(win->hwndFrame, _TRA("Replace Library Paths"), _TRA("Old directory prefix"));
    if (!oldPrefix) return;
    Str newPrefix = PromptLibraryText(win->hwndFrame, _TRA("Replace Library Paths"), _TRA("New directory prefix"));
    if (!newPrefix) {
        str::Free(oldPrefix);
        return;
    }
    Vec<LibraryPathChange*> changes = LibraryStorePreviewPathReplace(LibraryGetStore(), oldPrefix, newPrefix);
    str::Builder preview;
    int missing = 0;
    int conflicts = 0;
    int opened = 0;
    for (LibraryPathChange* change : changes) {
        bool isOpen = IsLibraryPathOpen(change->oldPath);
        if (!change->targetExists) missing++;
        if (change->conflict) conflicts++;
        if (isOpen) opened++;
        preview.Append(fmt("%s\n  -> %s%s%s%s\n\n", change->oldPath, change->newPath,
                           change->targetExists ? Str() : _TRA("  [target missing]"),
                           change->conflict ? _TRA("  [path conflict]") : Str(),
                           isOpen ? _TRA("  [currently open]") : Str()));
    }
    preview.Append(fmt(_TRA("Books: %d, missing targets: %d, conflicts: %d, currently open: %d\n").s, len(changes),
                       missing, conflicts, opened));
    ShowTextInWindowDialog(_TRA("Library Path Replacement Preview"), ToStr(preview));
    bool canApply = len(changes) > 0 && conflicts == 0 && opened == 0;
    if (!canApply) {
        MessageBoxW(
            win->hwndFrame,
            CWStrTemp(_TRA("No paths can be changed, or the preview contains a conflict/currently open document.")),
            CWStrTemp(_TRA("Library Paths")), MB_OK | MB_ICONWARNING);
    } else if (MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("Apply all previewed path changes in one transaction?")),
                           CWStrTemp(_TRA("Library Paths")), MB_YESNO | MB_ICONQUESTION) == IDYES) {
        if (!LibraryStoreApplyPathReplace(LibraryGetStore(), changes)) {
            MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("The path replacement failed. No paths were changed.")),
                        CWStrTemp(_TRA("Library Paths")), MB_OK | MB_ICONERROR);
        } else {
            logf("Library path replacement: %d paths, %d missing targets\n", len(changes), missing);
            RefreshLibraryPanels();
        }
    }
    DeleteLibraryPathChanges(changes);
    str::Free(oldPrefix);
    str::Free(newPrefix);
}

static void ImportPdfDirectory(MainWindow* win) {
    BROWSEINFOW bi{};
    bi.hwndOwner = win->hwndFrame;
    bi.lpszTitle = CWStrTemp(_TRA("Import PDF directory"));
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    WCHAR pathW[MAX_PATH]{};
    bool ok = SHGetPathFromIDListW(pidl, pathW);
    CoTaskMemFree(pidl);
    if (!ok) return;
    TempStr dir = ToUtf8Temp(pathW);
    DirIter iter(dir);
    iter.recurse = true;
    int imported = 0;
    for (DirIterEntry* entry : iter) {
        if (!entry->isFile || !str::EndsWithI(entry->filePath, StrL(".pdf"))) continue;
        if (AddPdfToLibrary(win, entry->filePath)) imported++;
    }
    logf("Library directory import: '%s', %d PDFs\n", dir, imported);
    RefreshLibraryPanels();
}

static void AddMenu(MainWindow* win, VirtMouseEvent* ev) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kLibraryAddShelf, CWStrTemp(_TRA("New Shelf")));
    AppendMenuW(menu, MF_STRING, kLibraryAddCategory, CWStrTemp(_TRA("New Category")));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLibraryAddPdf, CWStrTemp(_TRA("Add PDF...")));
    AppendMenuW(menu, MF_STRING, kLibraryImportDir, CWStrTemp(_TRA("Import PDF Directory...")));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLibraryReplacePath, CWStrTemp(_TRA("Replace Path Prefix...")));
    Point pt = HwndClientToScreen(win->hwndLibraryBox, ev->pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    DestroyMenu(menu);
    if (cmd == kLibraryAddShelf) {
        Str name = PromptLibraryText(win->hwndFrame, _TRA("New Shelf"), _TRA("Shelf name"));
        DeleteLibraryCollection(LibraryStoreCreateCollection(LibraryGetStore(), 0, true, name));
        str::Free(name);
    } else if (cmd == kLibraryAddCategory) {
        i64 parentId = SelectedCollectionId(win);
        if (parentId) {
            Str name = PromptLibraryText(win->hwndFrame, _TRA("New Category"), _TRA("Category name"));
            DeleteLibraryCollection(LibraryStoreCreateCollection(LibraryGetStore(), parentId, false, name));
            str::Free(name);
        }
    } else if (cmd == kLibraryAddPdf) {
        AddPdfFiles(win);
    } else if (cmd == kLibraryImportDir) {
        ImportPdfDirectory(win);
    } else if (cmd == kLibraryReplacePath) {
        ReplaceLibraryPathPrefix(win);
    }
    if (cmd == kLibraryAddShelf || cmd == kLibraryAddCategory) RefreshLibraryPanels();
}

void LayoutLibraryPanel(MainWindow* win) {
    if (!win || !win->libraryLayout || !win->hwndLibraryBox) return;
    Rect rc = HwndClientRect(win->hwndLibraryBox);
    if (rc.IsEmpty()) return;
    LayoutTreeToSize(win->hwndLibraryBox, win->libraryLayout, {rc.dx, rc.dy}, &win->libraryRoot);
}

static WNDPROC gLibraryBoxWndProc = nullptr;

static LRESULT CALLBACK LibraryBoxWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) return CallWindowProcW(gLibraryBoxWndProc, hwnd, msg, wp, lp);
    LRESULT result = 0;
    result = TryReflectMessages(hwnd, msg, wp, lp);
    if (result) return result;
    if (VirtHostOnMessage(hwnd, win->libraryRoot, msg, wp, lp, result, ThemeControlBackgroundColor())) return result;
    if (msg == WM_SIZE) LayoutLibraryPanel(win);
    return CallWindowProcW(gLibraryBoxWndProc, hwnd, msg, wp, lp);
}

static VirtIconButton* HeaderButton(const char* svg, Str tooltip, const VirtMouseHandler& onClick) {
    auto* button = new VirtIconButton();
    button->pixmap = GetCachedPixmapForSvg(Str(svg), DpiScale(16), DpiScale(16));
    button->padding = Insets{5, 5, 5, 5};
    button->onClick = onClick;
    button->SetTooltip(tooltip);
    return button;
}

void CreateLibraryPanel(MainWindow* win) {
    DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    win->hwndLibraryBox = CreateWindowW(WC_STATICW, L"", style, 0, 0, gGlobalPrefs->libraryDx, 0, win->hwndFrame,
                                        nullptr, GetModuleHandleW(nullptr), nullptr);
    PlatformFont* labelFont = GetAppSidebarLabelFont();
    auto header = NewLabelWithClose(win->hwndLibraryBox, labelFont, MkFunc1(CloseLibrary, win));
    win->libraryLabel = header.label;
    header.label->SetText(_TRA("Library"));

    auto* actions = new HBox();
    actions->alignCross = CrossAxisAlign::CrossCenter;
    win->libraryAddButton = HeaderButton(gIconPlus, _TRA("Add to Library"), MkFunc1(AddMenu, win));
    actions->AddChild(win->libraryAddButton);
    actions->AddChild(new Spacer(0, 0), 1);

    auto* filter = new Edit();
    Edit::CreateArgs editArgs;
    editArgs.parent = win->hwndLibraryBox;
    editArgs.withBorder = true;
    editArgs.cueText = _TRA("Search books or paths");
    editArgs.font = GetAppFont();
    filter->Create(editArgs);
    filter->onTextChanged = MkFunc0(OnFilterChanged, win);
    win->libraryFilterEdit = filter;

    auto* tree = new TreeView();
    TreeView::CreateArgs treeArgs;
    treeArgs.parent = win->hwndLibraryBox;
    treeArgs.font = GetAppTreeFont();
    treeArgs.fullRowSelect = true;
    treeArgs.isRtl = IsUIRtl();
    tree->onClick = MkFunc1Void(OnTreeClick);
    tree->onContextMenu = MkFunc1Void(OnTreeContextMenu);
    tree->onGetTooltip = MkFunc1Void(OnTreeTooltip);
    tree->Create(treeArgs);
    // Keep the tree's client width stable while switching documents. The
    // library model changes the number of rows, and letting Windows add/remove
    // the vertical scrollbar changes the pane's right edge by one scrollbar
    // width, which makes the adjacent chrome appear to shift.
    LONG_PTR treeStyle = GetWindowLongPtrW(tree->hwnd, GWL_STYLE);
    SetWindowLongPtrW(tree->hwnd, GWL_STYLE, treeStyle | WS_VSCROLL);
    ShowScrollBar(tree->hwnd, SB_VERT, FALSE);
    win->libraryTreeView = tree;
    SetWindowSubclass(tree->hwnd, LibraryTreeSubclassProc, NextSubclassId(), (DWORD_PTR)win);

    auto* layout = new VBox();
    layout->alignCross = CrossAxisAlign::Stretch;
    layout->AddChild(header.box);
    layout->AddChild(actions);
    layout->AddChild(filter);
    layout->AddChild(new Spacer(0, 2));
    layout->AddChild(tree, 1);
    win->libraryLayout = layout;

    if (!gLibraryBoxWndProc) gLibraryBoxWndProc = (WNDPROC)GetWindowLongPtrW(win->hwndLibraryBox, GWLP_WNDPROC);
    SetWindowLongPtrW(win->hwndLibraryBox, GWLP_WNDPROC, (LONG_PTR)LibraryBoxWndProc);
    RefreshLibraryPanel(win);
    UpdateControlsColors(win);
}

void UpdateLibraryPanelText(MainWindow* win) {
    if (!win) return;
    if (win->libraryLabel) {
        win->libraryLabel->SetText(_TRA("Library"));
        win->libraryLabel->Invalidate();
    }
    if (win->libraryFilterEdit) {
        win->libraryFilterEdit->SetCue(_TRA("Search books or paths"));
    }
    if (win->libraryAddButton) {
        win->libraryAddButton->SetTooltip(_TRA("Add to Library"));
    }
    RefreshLibraryPanel(win);
}

void SetLibraryPanelVisible(MainWindow* win, bool visible) {
    if (!LibraryIsAvailable()) visible = false;
    gGlobalPrefs->showLibrary = visible;
    win->uiState.libraryVisible = visible;
    if (!visible && win->libraryTreeView && HwndIsFocused(win->libraryTreeView->hwnd)) HwndSetFocus(win->hwndFrame);
    SaveSettings();
    ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars | kUiSidebarDirty);
}
