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

#include "FilterHighlightDraw.h"
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
    bool isShelf = false;
    bool expanded = false;
    u32 bgColor = 0;
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
    Vec<LibraryBook*> books = LibraryStoreGetBooks(LibraryGetStore(), scope, collectionId, LibrarySort::Manual, filter);
    for (LibraryBook* book : books) {
        auto* item = NewItem(parent, LibraryTreeKind::Book, book->title);
        item->bookId = book->id;
        item->path = str::Dup(book->path);
        item->bgColor = book->bgColor;
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

    Vec<LibraryCollection*> collections = LibraryStoreGetCollections(LibraryGetStore());
    Vec<LibraryTreeItem*> collectionItems;
    for (LibraryCollection* collection : collections) {
        auto* item = new LibraryTreeItem();
        item->kind = LibraryTreeKind::Collection;
        item->text = str::Dup(collection->name);
        item->collectionId = collection->id;
        item->isShelf = collection->isShelf;
        item->bgColor = collection->bgColor;
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
    AddBooks(model->root, LibraryBookScope::ManualRoot, 0, filter);
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

static bool LibraryPathsEqual(Str a, Str b) {
    if (!a || !b) {
        return false;
    }
    return str::EqI(path::NormalizeTemp(a), path::NormalizeTemp(b));
}

static LibraryTreeItem* FindBookByPath(LibraryTreeItem* item, Str path) {
    if (!item || !path) {
        return nullptr;
    }
    if (item->kind == LibraryTreeKind::Book && LibraryPathsEqual(item->path, path)) {
        return item;
    }
    for (LibraryTreeItem* child : item->children) {
        LibraryTreeItem* found = FindBookByPath(child, path);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

static Str CurrentPdfPath(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!IsPdfTab(tab)) {
        return Str();
    }
    return tab->filePath;
}

// TreeView selection is the "currently viewed book" highlight. Rebuilds and
// tab switches must restore it from the current tab path; otherwise Windows
// leaves no selection or keeps the first inserted row blue.
void SyncLibrarySelection(MainWindow* win) {
    if (!win || !win->libraryTreeView || !win->libraryTreeView->treeModel) {
        return;
    }
    auto* model = (LibraryTreeModel*)win->libraryTreeView->treeModel;
    Str path = CurrentPdfPath(win);
    LibraryTreeItem* book = FindBookByPath(model->root, path);
    TreeItem want = book ? (TreeItem)book : TreeModel::kNullItem;
    if (win->libraryTreeView->GetSelection() == want) {
        return;
    }
    logf("SyncLibrarySelection: path='%s' book=%d\n", path ? path : StrL(""), book ? 1 : 0);
    win->libraryTreeView->SelectItem(want);
    if (!book) {
        return;
    }
    HTREEITEM hi = win->libraryTreeView->GetHandleByTreeItem((TreeItem)book);
    if (!hi) {
        return;
    }
    RECT itemRc{};
    RECT clientRc{};
    if (TreeView_GetItemRect(win->libraryTreeView->hwnd, hi, &itemRc, FALSE) &&
        GetClientRect(win->libraryTreeView->hwnd, &clientRc)) {
        if (itemRc.top >= 0 && itemRc.bottom <= clientRc.bottom) {
            return;
        }
    }
    TreeView_EnsureVisible(win->libraryTreeView->hwnd, hi);
}

void RefreshLibraryPanel(MainWindow* win) {
    if (!win || !win->libraryTreeView) return;
    logf("RefreshLibraryPanel: begin\n");
    if (win->libraryDragging) ReleaseCapture();
    win->libraryDragItem = 0;
    win->libraryDropItem = 0;
    win->libraryDragging = false;
    win->libraryDropAfter = false;
    RememberExpandedCollections(win);
    TempStr filter = FilterTextTemp(win);
    TreeModel* previous = win->libraryTreeView->treeModel;
    win->libraryTreeView->SetTreeModel(BuildModel(win, filter));
    win->libraryModelFiltered = !!filter;
    win->libraryExpansionInitialized = true;
    delete previous;
    SyncLibrarySelection(win);
    logf("RefreshLibraryPanel: end\n");
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
        if (existing != source) {
            SyncLibrarySelection(source);
        }
        return;
    }
    LoadArgs args(item->path, source);
    StartLoadDocument(&args);
}

static void OnTreeTooltip(TreeView::GetTooltipEvent* ev) {
    auto* item = (LibraryTreeItem*)ev->treeItem;
    if (item && item->path) {
        str::BufSet(ev->info->pszText, ev->info->cchTextMax, item->path);
    }
}

static void SplitBookLabel(LibraryTreeItem* item, Str& stem, Str& ext) {
    Str name = item && item->text ? item->text : Str();
    if (str::EndsWithI(name, StrL(".pdf"))) {
        stem = Str(name.s, len(name) - 4);
        ext = StrL(".pdf");
        return;
    }
    stem = name;
    ext = item && item->path && str::EndsWithI(item->path, StrL(".pdf")) ? StrL(".pdf") : Str();
}

static void DrawLibraryFade(Gfx* gfx, Rect rc, Color bg) {
    int fadeDx = std::min(rc.dx, DpiScale(22));
    if (fadeDx <= 0) {
        return;
    }
    constexpr int kSteps = 8;
    for (int i = 0; i < kSteps; i++) {
        int x0 = rc.x + rc.dx - fadeDx + fadeDx * i / kSteps;
        int x1 = rc.x + rc.dx - fadeDx + fadeDx * (i + 1) / kSteps;
        Rect strip{x0, rc.y, std::max(1, x1 - x0), rc.dy};
        gfx->FillRects(&strip, 1, bg, (u8)(255 * (i + 1) / kSteps));
    }
}

static void DrawLibraryItem(TreeView::CustomDrawEvent* ev, MainWindow* win) {
    auto* item = (LibraryTreeItem*)ev->treeItem;
    if (!item) {
        return;
    }
    TreeView* tv = ev->treeView;
    Rect labelRect{};
    if (!tv->GetItemRect(ev->treeItem, true, labelRect)) {
        return;
    }
    Rect itemRect{};
    tv->GetItemRect(ev->treeItem, false, itemRect);

    NMTVCUSTOMDRAW* tvcd = ev->nm;
    HDC hdc = tvcd->nmcd.hdc;
    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
    if (!isSelected) {
        HTREEITEM hSel = TreeView_GetSelection(tv->hwnd);
        HTREEITEM hItem = tv->GetHandleByTreeItem(ev->treeItem);
        isSelected = hSel && hItem && hSel == hItem;
    }
    bool isDrop = win && win->libraryDropItem && win->libraryDropItem == (uintptr_t)item;
    bool dropAsSibling = isDrop && item->kind == LibraryTreeKind::Book && win->libraryDragItem &&
                         ((LibraryTreeItem*)win->libraryDragItem)->kind == LibraryTreeKind::Book;
    bool hasFocus = isSelected && GetFocus() == tv->hwnd;
    Color bgCol, txtCol;
    ResolveTreeFilterItemColors(hdc, itemRect, tv->bgColor, tv->textColor, isSelected || (isDrop && !dropAsSibling),
                                hasFocus, &bgCol, &txtCol);
    // Song-inspired pale row tint when not selected/drop-highlighted.
    if (item->bgColor && !isSelected && !(isDrop && !dropAsSibling)) {
        u8 r = (u8)((item->bgColor >> 16) & 0xff);
        u8 g = (u8)((item->bgColor >> 8) & 0xff);
        u8 b = (u8)(item->bgColor & 0xff);
        bgCol = MkRgb(r, g, b);
    }

    RECT client{};
    GetClientRect(tv->hwnd, &client);
    RECT drawRc = ToRECT(labelRect);
    drawRc.top = itemRect.dy > 0 ? itemRect.y : drawRc.top;
    drawRc.bottom = itemRect.dy > 0 ? itemRect.y + itemRect.dy : drawRc.bottom;
    drawRc.right = client.right;
    if (drawRc.right <= drawRc.left) {
        return;
    }
    Rect drawRect = ToRect(drawRc);
    GfxHdc gfx(hdc);
    // Cover the default TreeView label completely. POSTPAINT used to fade over
    // it, so the tail of the filename stayed visible under the pinned ".pdf".
    gfx.FillRect(drawRect, bgCol);
    Rect textRect = drawRect;
    textRect.Inflate(-2, -1);

    if (item->kind == LibraryTreeKind::Book) {
        Str stem, ext;
        SplitBookLabel(item, stem, ext);
        Size extSize = ext ? gfx.MeasureText(ext, tv->GetFont()) : Size{};
        int extDx = ext ? extSize.dx : 0;
        Rect stemRect = textRect;
        stemRect.dx = std::max(0, textRect.dx - extDx);
        Size stemSize = stem ? gfx.MeasureText(stem, tv->GetFont()) : Size{};
        if (stem && stemRect.dx > 0) {
            int saved = SaveDC(hdc);
            IntersectClipRect(hdc, stemRect.x, stemRect.y, stemRect.x + stemRect.dx, stemRect.y + stemRect.dy);
            gfx.DrawText(stem, stemRect, gfxTextVCenter | gfxTextEllipsis, tv->GetFont(), txtCol);
            RestoreDC(hdc, saved);
        }
        if (stemSize.dx > stemRect.dx) {
            DrawLibraryFade(&gfx, stemRect, bgCol);
        }
        if (ext) {
            Rect extRect = textRect;
            extRect.x = stemRect.x + stemRect.dx;
            extRect.dx = extDx;
            gfx.FillRect(extRect, bgCol);
            gfx.DrawText(ext, extRect, gfxTextVCenter | gfxTextEllipsis, tv->GetFont(), txtCol);
        }
    } else if (item->kind == LibraryTreeKind::Collection) {
        // Folder emoji distinguishes shelves/categories from book rows.
        TempStr labeled = fmt("📁 %s", item->text ? item->text : StrL(""));
        gfx.DrawText(labeled, textRect, gfxTextVCenter | gfxTextEllipsis, tv->GetFont(), txtCol);
    } else {
        gfx.DrawText(item->text, textRect, gfxTextVCenter | gfxTextEllipsis, tv->GetFont(), txtCol);
    }
    if (dropAsSibling) {
        int y = win->libraryDropAfter ? drawRect.y + drawRect.dy - 1 : drawRect.y;
        Rect line{drawRect.x, y, drawRect.dx, 2};
        gfx.FillRect(line, txtCol);
    } else if (isDrop && !isSelected) {
        gfx.DrawRect(drawRect, txtCol);
    }
}

static void OnLibraryCustomDraw(TreeView::CustomDrawEvent* ev) {
    ev->result = CDRF_DODEFAULT;
    NMCUSTOMDRAW* cd = &ev->nm->nmcd;
    if (cd->dwDrawStage == CDDS_PREPAINT) {
        ev->result = CDRF_NOTIFYITEMDRAW;
        return;
    }
    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        // Hide the default label; we paint stem + pinned extension ourselves.
        ev->nm->clrText = ev->nm->clrTextBk;
        ev->result = CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
        return;
    }
    if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT) {
        DrawLibraryItem(ev, FindMainWindowByHwnd(ev->treeView->hwnd));
        ev->result = CDRF_DODEFAULT;
    }
}

static i64 CollectionIdForItem(LibraryTreeItem* item) {
    if (!item) {
        return 0;
    }
    if (item->kind == LibraryTreeKind::Collection) {
        return item->collectionId;
    }
    if (item->kind == LibraryTreeKind::Book && item->parent && item->parent->kind == LibraryTreeKind::Collection) {
        return item->parent->collectionId;
    }
    return 0;
}

static void SetLibraryDropItem(MainWindow* win, TreeItem item, bool dropAfter) {
    if (!win || (win->libraryDropItem == (uintptr_t)item && win->libraryDropAfter == dropAfter)) {
        return;
    }
    win->libraryDropItem = (uintptr_t)item;
    win->libraryDropAfter = dropAfter;
    if (win->libraryTreeView) {
        HwndInvalidate(win->libraryTreeView->hwnd);
    }
}

enum {
    kLibraryMenuOpenFolder = 1,
    kLibraryMenuRemoveBook,
    kLibraryMenuDeleteCollection,
    kLibraryMenuRenameCollection,
    kLibraryMenuNewCategory,
    kLibraryMenuColorNone,
    kLibraryMenuColorFirst,
};

// Pale Song-dynasty inspired row tints (flat, low chroma). Values are 0x00RRGGBB.
struct LibraryBgSwatch {
    const char* name;
    u32 rgb;
};

static const LibraryBgSwatch gLibraryBgSwatches[] = {
    {"月白", 0xD6ECF0}, {"青白", 0xC0EBD7}, {"天水碧", 0xD5EBED}, {"艾绿", 0xD5EFDF},
    {"竹青", 0xE2EAD9}, {"水色", 0xDCE8E6}, {"藕荷", 0xF0E0E6}, {"藕色", 0xF5E6EA},
    {"缃色", 0xF4ECD4},
};

static HBITMAP CreateFlatColorSwatch(u32 rgb, int size) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        return nullptr;
    }
    u8 r = (u8)((rgb >> 16) & 0xff);
    u8 g = (u8)((rgb >> 8) & 0xff);
    u8 b = (u8)(rgb & 0xff);
    // Slightly deeper edge for a flat chip outline without shadows.
    u8 er = (u8)(r * 85 / 100);
    u8 eg = (u8)(g * 85 / 100);
    u8 eb = (u8)(b * 85 / 100);
    auto* px = (u32*)bits;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            bool edge = x == 0 || y == 0 || x == size - 1 || y == size - 1;
            u8 cr = edge ? er : r;
            u8 cg = edge ? eg : g;
            u8 cb = edge ? eb : b;
            // BGRA8 in memory: B | G<<8 | R<<16 | A<<24
            px[y * size + x] = cb | ((u32)cg << 8) | ((u32)cr << 16) | (0xffu << 24);
        }
    }
    return hbmp;
}

static void AppendLibraryColorMenu(HMENU parent, Vec<HBITMAP>& bitmaps) {
    HMENU colorMenu = CreatePopupMenu();
    AppendMenuW(colorMenu, MF_STRING, kLibraryMenuColorNone, CWStrTemp(_TRA("None")));
    AppendMenuW(colorMenu, MF_SEPARATOR, 0, nullptr);
    int swatchSize = DpiScale(22);
    for (int i = 0; i < dimof(gLibraryBgSwatches); i++) {
        HBITMAP hbmp = CreateFlatColorSwatch(gLibraryBgSwatches[i].rgb, swatchSize);
        if (hbmp) {
            bitmaps.Append(hbmp);
        }
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP | MIIM_FTYPE;
        mii.fType = MFT_STRING;
        mii.wID = kLibraryMenuColorFirst + i;
        mii.dwTypeData = (LPWSTR)CWStrTemp(gLibraryBgSwatches[i].name);
        mii.hbmpItem = hbmp;
        InsertMenuItemW(colorMenu, GetMenuItemCount(colorMenu), TRUE, &mii);
    }
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)colorMenu, CWStrTemp(_TRA("Background color")));
}

static Str PromptLibraryText(HWND parent, Str title, Str label);
static bool CreateNamedCollection(MainWindow* win, i64 parentId, bool isShelf);

static void OnTreeContextMenu(ContextMenuEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    if (!win) return;
    TreeItem selected = win->libraryTreeView->GetItemAt(ev->mouseWindow.x, ev->mouseWindow.y);
    if (!selected) selected = win->libraryTreeView->GetSelection();
    auto* item = (LibraryTreeItem*)selected;
    if (!item) return;
    HMENU menu = CreatePopupMenu();
    Vec<HBITMAP> swatchBitmaps;
    if (item->kind == LibraryTreeKind::Book) {
        AppendMenuW(menu, MF_STRING, kLibraryMenuOpenFolder, CWStrTemp(_TRA("Show in folder")));
        AppendLibraryColorMenu(menu, swatchBitmaps);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuRemoveBook, CWStrTemp(_TRA("Remove from Library")));
    } else if (item->kind == LibraryTreeKind::Collection) {
        if (item->isShelf) {
            AppendMenuW(menu, MF_STRING, kLibraryMenuNewCategory, CWStrTemp(_TRA("New Category")));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendLibraryColorMenu(menu, swatchBitmaps);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuRenameCollection,
                    CWStrTemp(item->isShelf ? _TRA("Rename Shelf") : _TRA("Rename Category")));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuDeleteCollection,
                    CWStrTemp(item->isShelf ? _TRA("Delete Shelf") : _TRA("Delete Category")));
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ev->mouseScreen.x, ev->mouseScreen.y, 0,
                             win->hwndFrame, nullptr);
    DestroyMenu(menu);
    for (HBITMAP hbmp : swatchBitmaps) {
        DeleteObject(hbmp);
    }
    bool changed = false;
    if (cmd == kLibraryMenuOpenFolder) {
        if (item->path) {
            SumatraOpenPathInDefaultFileManager(item->path);
        }
    } else if (cmd == kLibraryMenuColorNone) {
        if (item->kind == LibraryTreeKind::Book) {
            changed = LibraryStoreSetBookBgColor(LibraryGetStore(), item->bookId, 0);
        } else if (item->kind == LibraryTreeKind::Collection) {
            changed = LibraryStoreSetCollectionBgColor(LibraryGetStore(), item->collectionId, 0);
        }
    } else if (cmd >= kLibraryMenuColorFirst && cmd < kLibraryMenuColorFirst + dimof(gLibraryBgSwatches)) {
        u32 rgb = gLibraryBgSwatches[cmd - kLibraryMenuColorFirst].rgb;
        if (item->kind == LibraryTreeKind::Book) {
            changed = LibraryStoreSetBookBgColor(LibraryGetStore(), item->bookId, rgb);
        } else if (item->kind == LibraryTreeKind::Collection) {
            changed = LibraryStoreSetCollectionBgColor(LibraryGetStore(), item->collectionId, rgb);
        }
    } else if (cmd == kLibraryMenuRemoveBook) {
        changed = LibraryStoreRemoveBook(LibraryGetStore(), item->bookId);
    } else if (cmd == kLibraryMenuDeleteCollection) {
        changed = LibraryStoreDeleteCollection(LibraryGetStore(), item->collectionId);
    } else if (cmd == kLibraryMenuRenameCollection) {
        Str name = PromptLibraryText(win->hwndFrame, item->isShelf ? _TRA("Rename Shelf") : _TRA("Rename Category"),
                                     _TRA("New name"));
        if (name) {
            changed = LibraryStoreRenameCollection(LibraryGetStore(), item->collectionId, name);
            str::Free(name);
        }
    } else if (cmd == kLibraryMenuNewCategory) {
        changed = CreateNamedCollection(win, item->collectionId, false);
    }
    if (changed) RefreshLibraryPanels();
}

static LRESULT CALLBACK LibraryTreeSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    MainWindow* win = (MainWindow*)data;
    if (msg == WM_LBUTTONDOWN && win && win->libraryTreeView) {
        TVHITTESTINFO ht{};
        ht.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        TreeView_HitTest(hwnd, &ht);
        // +/- is expand, not a drag handle.
        win->libraryDragItem = (ht.flags & TVHT_ONITEMBUTTON) ? 0 : win->libraryTreeView->GetTreeItemByHandle(ht.hItem);
        win->libraryDragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        win->libraryDragging = false;
        SetLibraryDropItem(win, 0, false);
    }
    if (msg == WM_MOUSEMOVE && win && win->libraryDragItem && (wp & MK_LBUTTON)) {
        int dx = std::abs(GET_X_LPARAM(lp) - win->libraryDragStart.x);
        int dy = std::abs(GET_Y_LPARAM(lp) - win->libraryDragStart.y);
        if (!win->libraryDragging && (dx >= 3 || dy >= 3)) {
            win->libraryDragging = true;
            SetCapture(hwnd);
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        if (win->libraryDragging) {
            RECT clientRc{};
            GetClientRect(hwnd, &clientRc);
            int y = GET_Y_LPARAM(lp);
            int edge = DpiScale(28);
            if (y < edge) {
                SendMessageW(hwnd, WM_VSCROLL, SB_LINEUP, 0);
            } else if (y > clientRc.bottom - edge) {
                SendMessageW(hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
            }
            auto* hover = (LibraryTreeItem*)win->libraryTreeView->GetItemAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hover && hover->kind == LibraryTreeKind::Collection) {
                HTREEITEM hi = win->libraryTreeView->GetHandleByTreeItem((TreeItem)hover);
                if (hi) {
                    TreeView_Expand(hwnd, hi, TVE_EXPAND);
                }
            }
            bool dropAfter = false;
            if (hover && hover->kind == LibraryTreeKind::Book) {
                Rect itemRc{};
                if (win->libraryTreeView->GetItemRect((TreeItem)hover, false, itemRc) && itemRc.dy > 0) {
                    dropAfter = GET_Y_LPARAM(lp) >= itemRc.y + itemRc.dy / 2;
                }
            }
            SetLibraryDropItem(win, (TreeItem)hover, dropAfter);
            return 0;
        }
    }
    if (msg == WM_MOUSEMOVE && win && !win->libraryDragging) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }
    if (msg == WM_MOUSELEAVE) {
        HwndInvalidate(hwnd);
    }
    if (msg == WM_LBUTTONUP && win && win->libraryTreeView) {
        auto* source = (LibraryTreeItem*)win->libraryDragItem;
        auto* target = (LibraryTreeItem*)win->libraryDropItem;
        bool dropAfter = win->libraryDropAfter;
        if (!target) {
            target = (LibraryTreeItem*)win->libraryTreeView->GetItemAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        }
        bool wasDragging = win->libraryDragging;
        bool filtered = win->libraryModelFiltered;
        bool changed = false;
        if (wasDragging && source && source != target && !filtered) {
            if (source->kind == LibraryTreeKind::Book && target && target->kind == LibraryTreeKind::Book) {
                i64 srcCol = CollectionIdForItem(source);
                i64 dstCol = CollectionIdForItem(target);
                if (srcCol == dstCol) {
                    changed = LibraryStoreReorderBook(LibraryGetStore(), source->bookId, srcCol, target->bookId,
                                                     dropAfter);
                } else {
                    changed = LibraryStorePlaceBook(LibraryGetStore(), source->bookId, srcCol, dstCol, false);
                    if (changed) {
                        changed = LibraryStoreReorderBook(LibraryGetStore(), source->bookId, dstCol, target->bookId,
                                                          dropAfter) ||
                                  changed;
                    }
                }
            } else if (source->kind == LibraryTreeKind::Book) {
                changed = LibraryStorePlaceBook(LibraryGetStore(), source->bookId, CollectionIdForItem(source),
                                                CollectionIdForItem(target), false);
            } else if (source->kind == LibraryTreeKind::Collection && source->isShelf && !target) {
                changed = LibraryStoreMoveCollection(LibraryGetStore(), source->collectionId, 0);
            } else if (source->kind == LibraryTreeKind::Collection && !source->isShelf && target &&
                       target->kind == LibraryTreeKind::Collection && target->isShelf) {
                changed = LibraryStoreMoveCollection(LibraryGetStore(), source->collectionId, target->collectionId);
            }
        }
        if (win->libraryDragging) {
            ReleaseCapture();
        }
        win->libraryDragItem = 0;
        win->libraryDragging = false;
        SetLibraryDropItem(win, 0, false);
        if (changed) {
            RefreshLibraryPanels();
            return 0;
        }
        if (!wasDragging && source && source->kind == LibraryTreeKind::Book) {
            OpenLibraryItem(win, source);
            return 0;
        }
        return 0;
    }
    if (msg == WM_CAPTURECHANGED && win) {
        win->libraryDragItem = 0;
        win->libraryDragging = false;
        SetLibraryDropItem(win, 0, false);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void CloseLibrary(MainWindow* win, VirtMouseEvent*) {
    SetLibraryPanelVisible(win, false);
}

enum {
    kLibraryAddShelf = 1,
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

static bool CreateNamedCollection(MainWindow* win, i64 parentId, bool isShelf) {
    Str title = isShelf ? _TRA("New Shelf") : _TRA("New Category");
    Str label = isShelf ? _TRA("Shelf name") : _TRA("Category name");
    Str name = PromptLibraryText(win->hwndFrame, title, label);
    if (!name) {
        return false;
    }
    LibraryCollection* created = LibraryStoreCreateCollection(LibraryGetStore(), parentId, isShelf, name);
    if (!created) {
        logf("Library failed to create %s '%s': %s\n", isShelf ? StrL("shelf") : StrL("category"), name,
             LibraryGetError());
        MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("Could not create the shelf or category.")), CWStrTemp(title),
                    MB_OK | MB_ICONWARNING);
        str::Free(name);
        return false;
    }
    logf("Library created %s '%s' id=%lld\n", isShelf ? StrL("shelf") : StrL("category"), name, created->id);
    DeleteLibraryCollection(created);
    str::Free(name);
    return true;
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
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLibraryAddPdf, CWStrTemp(_TRA("Add PDF...")));
    AppendMenuW(menu, MF_STRING, kLibraryImportDir, CWStrTemp(_TRA("Import PDF Directory...")));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLibraryReplacePath, CWStrTemp(_TRA("Replace Path Prefix...")));
    Point pt = HwndClientToScreen(win->hwndLibraryBox, ev->pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    DestroyMenu(menu);
    if (cmd == kLibraryAddShelf) {
        if (CreateNamedCollection(win, 0, true)) RefreshLibraryPanels();
    } else if (cmd == kLibraryAddPdf) {
        AddPdfFiles(win);
    } else if (cmd == kLibraryImportDir) {
        ImportPdfDirectory(win);
    } else if (cmd == kLibraryReplacePath) {
        ReplaceLibraryPathPrefix(win);
    }
}

void LayoutLibraryPanel(MainWindow* win) {
    if (!win || !win->libraryLayout || !win->hwndLibraryBox) return;
    Rect rc = HwndClientRect(win->hwndLibraryBox);
    if (rc.IsEmpty()) return;
    LayoutTreeToSize(win->hwndLibraryBox, win->libraryLayout, {rc.dx, rc.dy}, &win->libraryRoot);
}

static WNDPROC gLibraryBoxWndProc = nullptr;

static int LibraryLeftGutterDx() {
    return DpiScale(kLibraryLeftGutterDip);
}

static LRESULT CALLBACK LibraryBoxWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) return CallWindowProcW(gLibraryBoxWndProc, hwnd, msg, wp, lp);
    // The library HWND covers the frame's left resize strip. Let the parent
    // see that edge so the cursor can become HTLEFT.
    if (msg == WM_NCHITTEST) {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.x >= 0 && pt.x < LibraryLeftGutterDx()) {
            return HTTRANSPARENT;
        }
    }
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
    int pad = DpiScale(2);
    button->padding = Insets{0, pad, 0, pad};
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
    win->libraryAddButton = HeaderButton(gIconPlus, _TRA("Add to Library"), MkFunc1(AddMenu, win));
    // Insert + immediately before the close button so the search field sits
    // on the same row as the bookmarks search (no extra action row).
    if (len(header.box->children) > 0) {
        header.box->children.Pop();
    }
    header.box->AddChild(win->libraryAddButton);
    header.box->AddChild(header.closeBtn);

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
    tree->onContextMenu = MkFunc1Void(OnTreeContextMenu);
    tree->onGetTooltip = MkFunc1Void(OnTreeTooltip);
    tree->onCustomDraw = MkFunc1Void(OnLibraryCustomDraw);
    tree->Create(treeArgs);
    LONG_PTR noDrag = GetWindowLongPtrW(tree->hwnd, GWL_STYLE);
    SetWindowLongPtrW(tree->hwnd, GWL_STYLE, noDrag | TVS_DISABLEDRAGDROP);
    // Keep the tree's client width stable while switching documents. The
    // library model changes the number of rows, and letting Windows add/remove
    // the vertical scrollbar changes the pane's right edge by one scrollbar
    // width, which makes the adjacent chrome appear to shift.
    LONG_PTR treeStyle = GetWindowLongPtrW(tree->hwnd, GWL_STYLE);
    SetWindowLongPtrW(tree->hwnd, GWL_STYLE, treeStyle | WS_VSCROLL);
    ShowScrollBar(tree->hwnd, SB_VERT, TRUE);
    win->libraryTreeView = tree;
    SetWindowSubclass(tree->hwnd, LibraryTreeSubclassProc, NextSubclassId(), (DWORD_PTR)win);

    auto* layout = new VBox();
    layout->alignCross = CrossAxisAlign::Stretch;
    layout->AddChild(header.box);
    layout->AddChild(filter);
    layout->AddChild(new Spacer(0, 2));
    layout->AddChild(tree, 1);
    win->libraryLayout = new Padding(layout, Insets{0, 0, 0, LibraryLeftGutterDx()});

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
    bool toc = win->CurrentTab() && win->CurrentTab()->showToc;
    logf("SetLibraryPanelVisible: visible=%d tocPref=%d\n", visible ? 1 : 0, toc ? 1 : 0);
    SetSidebarVisibility(win, toc, gGlobalPrefs->showFavorites);
}

static void AppendLibraryStatus(str::Builder& out, MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    Str path = tab && tab->filePath ? tab->filePath : Str();
    out.Append(fmt("libraryVis=%d tocVis=%d showToc=%d libraryDx=%d sidebarDx=%d tabs=%d path=%s\n",
                   win && win->uiState.libraryVisible ? 1 : 0, win && win->uiState.tocVisible ? 1 : 0,
                   tab && tab->showToc ? 1 : 0, win ? win->libraryDx : 0, win ? win->sidebarDx : 0,
                   win ? win->TabCount() : 0, path ? path : StrL("")));
}

TempStr LibraryDbgControlTemp(Str action, Str a, Str b, int n1, int n2, int* exitCodeOut) {
    str::Builder out;
    auto finish = [&](Str msg, int code) -> TempStr {
        if (msg) {
            out.Append(msg);
        }
        if (!str::EndsWith(ToStr(out), StrL("\n"))) {
            out.AppendChar('\n');
        }
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    if (len(gWindows) == 0) {
        return finish(StrL("NOTREADY no-window"), 2);
    }
    MainWindow* win = gWindows[0];
    if (!win) {
        return finish(StrL("NOTREADY no-window"), 2);
    }

    if (!action || str::EqI(action, "status")) {
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "show") || str::EqI(action, "hide")) {
        SetLibraryPanelVisible(win, str::EqI(action, "show"));
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), win->uiState.libraryVisible == str::EqI(action, "show") ? 0 : 1);
    }

    if (str::EqI(action, "toc-show") || str::EqI(action, "toc-hide")) {
        bool want = str::EqI(action, "toc-show");
        SetSidebarVisibility(win, want, gGlobalPrefs->showFavorites);
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), win->uiState.tocVisible == want ? 0 : 1);
    }

    if (str::EqI(action, "refresh")) {
        RefreshLibraryPanel(win);
        return finish(StrL("OK refreshed"), 0);
    }

    if (str::EqI(action, "open")) {
        if (!a) {
            return finish(StrL("ERROR open expects path"), 1);
        }
        logf("LibraryDbg: open '%s'\n", a);
        LoadArgs args(a, win);
        args.activateExisting = true;
        MainWindow* loaded = LoadDocument(&args);
        if (!loaded) {
            return finish(fmt("ERROR open-failed path=%s", a), 1);
        }
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "switch-tab")) {
        int nTabs = win->TabCount();
        if (n1 < 0 || n1 >= nTabs) {
            return finish(fmt("ERROR bad-tab idx=%d tabs=%d", n1, nTabs), 1);
        }
        logf("LibraryDbg: switch-tab %d\n", n1);
        ULONGLONG t0 = GetTickCount64();
        TabsSelect(win, n1);
        out.Append(fmt("OK idx=%d elapsedMs=%llu ", n1, GetTickCount64() - t0));
        AppendLibraryStatus(out, win);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "tabs")) {
        out.Append(fmt("OK count=%d\n", win->TabCount()));
        for (int i = 0; i < win->TabCount(); i++) {
            WindowTab* tab = win->GetTab(i);
            Str path = tab && tab->filePath ? tab->filePath : StrL("");
            out.Append(fmt("tab idx=%d current=%d path=%s\n", i, tab == win->CurrentTab() ? 1 : 0, path));
        }
        return finish(Str(), 0);
    }

    if (str::EqI(action, "list")) {
        if (!LibraryIsAvailable()) {
            return finish(StrL("ERROR library-unavailable"), 1);
        }
        Vec<LibraryCollection*> cols = LibraryStoreGetCollections(LibraryGetStore());
        Vec<LibraryBook*> books =
            LibraryStoreGetBooks(LibraryGetStore(), LibraryBookScope::All, 0, LibrarySort::Title, Str());
        out.Append(fmt("OK collections=%d books=%d\n", len(cols), len(books)));
        for (LibraryCollection* c : cols) {
            out.Append(fmt("col id=%lld parent=%lld shelf=%d name=%s\n", c->id, c->parentId, c->isShelf ? 1 : 0,
                           c->name ? c->name : StrL("")));
        }
        for (LibraryBook* book : books) {
            out.Append(fmt("book id=%lld title=%s path=%s\n", book->id, book->title ? book->title : StrL(""),
                           book->path ? book->path : StrL("")));
        }
        DeleteLibraryCollections(cols);
        DeleteLibraryBooks(books);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "new-shelf")) {
        if (!a) {
            return finish(StrL("ERROR new-shelf expects name"), 1);
        }
        LibraryCollection* created = LibraryStoreCreateCollection(LibraryGetStore(), 0, true, a);
        if (!created) {
            return finish(fmt("ERROR create-shelf name=%s err=%s", a, LibraryGetError()), 1);
        }
        i64 id = created->id;
        DeleteLibraryCollection(created);
        RefreshLibraryPanels();
        return finish(fmt("OK shelf=%lld name=%s", id, a), 0);
    }

    if (str::EqI(action, "place")) {
        if (n1 <= 0 || n2 < 0) {
            return finish(StrL("ERROR place expects bookId targetCollectionId"), 1);
        }
        bool ok = LibraryStorePlaceBook(LibraryGetStore(), n1, 0, n2, false);
        if (!ok) {
            return finish(fmt("ERROR place book=%d dest=%d err=%s", n1, n2, LibraryGetError()), 1);
        }
        RefreshLibraryPanels();
        return finish(fmt("OK placed book=%d dest=%d", n1, n2), 0);
    }

    if (str::EqI(action, "reorder")) {
        // n1=bookId, n2=targetBookId, a="after"|"before", b=collectionId (optional, default 0)
        if (n1 <= 0 || n2 <= 0) {
            return finish(StrL("ERROR reorder expects bookId targetBookId [after|before] [collectionId]"), 1);
        }
        bool insertAfter = a && str::EqI(a, "after");
        i64 collectionId = 0;
        if (b && b.s && b.s[0]) {
            collectionId = atoi(b.s);
        }
        bool ok = LibraryStoreReorderBook(LibraryGetStore(), n1, collectionId, n2, insertAfter);
        if (!ok) {
            return finish(fmt("ERROR reorder book=%d target=%d err=%s", n1, n2, LibraryGetError()), 1);
        }
        RefreshLibraryPanels();
        return finish(fmt("OK reordered book=%d target=%d after=%d col=%lld", n1, n2, insertAfter ? 1 : 0, collectionId),
                      0);
    }

    if (str::EqI(action, "stress-switch")) {
        Vec<int> docTabs;
        int nTabs = win->TabCount();
        for (int i = 0; i < nTabs; i++) {
            WindowTab* tab = win->GetTab(i);
            if (tab && tab->IsDocLoaded()) {
                docTabs.Append(i);
            }
        }
        if (len(docTabs) < 2) {
            return finish(fmt("ERROR need-doc-tabs have=%d", len(docTabs)), 1);
        }
        int rounds = n1 > 0 ? n1 : 20;
        if (rounds > 500) {
            rounds = 500;
        }
        int maxMs = 0;
        int hangAt = -1;
        logf("LibraryDbg: stress-switch rounds=%d docTabs=%d libraryVis=%d\n", rounds, len(docTabs),
             win->uiState.libraryVisible ? 1 : 0);
        for (int i = 0; i < rounds; i++) {
            ULONGLONG t0 = GetTickCount64();
            TabsSelect(win, docTabs[i % len(docTabs)]);
            int dt = (int)(GetTickCount64() - t0);
            if (dt > maxMs) {
                maxMs = dt;
            }
            if (dt > 5000) {
                hangAt = i;
                logf("LibraryDbg: stress-switch hang i=%d ms=%d\n", i, dt);
                break;
            }
        }
        out.Append(fmt("OK rounds=%d maxMs=%d hangAt=%d ", rounds, maxMs, hangAt));
        AppendLibraryStatus(out, win);
        return finish(Str(), hangAt >= 0 ? 1 : 0);
    }

    return finish(fmt("ERROR unknown-action action=%s", action), 1);
}
