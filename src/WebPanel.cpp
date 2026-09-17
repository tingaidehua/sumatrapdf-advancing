/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/Win.h"
#include "base/Http.h"
#include "base/Pixmap.h"
#include "base/UITask.h"
#include "base/DirScan.h"

#include "gui/Dpi.h"
#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"
#include "gui/GuiColors.h"
#include "gui/win/WebView.h"

#include "Settings.h"
#include "AppSettings.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Theme.h"
#include "DarkMode_win.h"
#include "SvgIcons.h"
#include "Translations.h"
#include "ImageReader.h"
#include "AIChatCommon.h"
#include "AIChatPanel.h"
#include "WebPanel.h"
#include "AppTools.h"
#include "Library.h"
#include "LibraryStore.h"
#include "base/JsonParser.h"

#include <psapi.h>
#include <tlhelp32.h>

namespace {

constexpr int kDefaultCdpPort = 9224;
// Match anything-copilot: iPhone Safari UA + narrow viewport (no hybrid desktop)
static const char* kMobileUserAgent =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1";

static const char* kMobileViewportScript =
    R"JS((function(){
  try {
    var m = document.querySelector('meta[name="viewport"]');
    if (!m) {
      m = document.createElement('meta');
      m.setAttribute('name', 'viewport');
      (document.head || document.documentElement).appendChild(m);
    }
    m.setAttribute('content', 'width=device-width, initial-scale=1, maximum-scale=1, viewport-fit=cover');
  } catch (e) {}
})())JS";

struct DefaultBookmark {
    const char* title;
    const char* url;
};

// Featured sites (ZIZIYI / anything-copilot style) + NotebookLM
static const DefaultBookmark kDefaultBookmarks[] = {
    {"Gemini Notebook",
     "https://notebook.google.com/?icid=NotebookLM_A_B_test_google_oo_website_AB_test_main_cta_variant_A"},
    {"ChatGPT", "https://chatgpt.com/"},
    {"Claude", "https://claude.ai/"},
    {"Gemini", "https://gemini.google.com/"},
    {"DeepSeek", "https://chat.deepseek.com/"},
    {"Kimi", "https://kimi.moonshot.cn/"},
    {"Doubao", "https://www.doubao.com/chat/"},
    {"Grok", "https://grok.x.ai/"},
    {"Perplexity", "https://www.perplexity.ai/"},
    {"Microsoft Copilot", "https://copilot.microsoft.com/"},
    {"X", "https://x.com/"},
    {"Reddit", "https://www.reddit.com/"},
    {"YouTube Music", "https://music.youtube.com/"},
    {"Apple Podcasts", "https://podcasts.apple.com/"},
};

struct WebBookmark {
    Str title;
    Str url;
    bool pinned = false;
};

Vec<WebBookmark> gBookmarks;
Str gLastUrl;
int gCdpPort = kDefaultCdpPort;
WNDPROC gWebPanelBoxWndProc = nullptr;

int FindBookmarkByUrl(Str url);
void SaveBookmarks();
void SaveWebPanelTabs(MainWindow* win);
void LoadWebPanelTabs(MainWindow* win);
void RememberPdfActiveTab(MainWindow* win);
void RestorePdfActiveTab(MainWindow* win);
void CreateNewWebPanelTab(MainWindow* win, Str url, Str title, Str forcedId = {});
void ActivateWebPanelTabByIndex(MainWindow* win, int idx, bool rememberPdf);
void CloseWebPanelTabAt(MainWindow* win, int idx);
void CloseAllWebPanelTabs(MainWindow* win);
void RestorePdfNotebookLmTab(MainWindow* win);
void ShowTabsMenu(MainWindow* win);
void UpdateWebPanelCpuSample();
TempStr WebPanelResourceStatsTemp();
void RebuildPinStrip(MainWindow* win);
void EnsureWebPanelWebView(MainWindow* win);
void DeleteBookmarkAt(MainWindow* win, int idx);
void ActivateWebPanelTab(MainWindow* win, Str url);
void ShowActiveWebPanelTab(MainWindow* win);
WebviewWnd* CreateWebPanelTabWebView(MainWindow* win, Str url);
bool OnWebNavStarting(void* ctx, Str url, bool newWindow);
void OnWebNavCompleted(void* ctx, Str url, bool success);
void OnWebSourceChanged(void* ctx, WebviewWnd* sender, Str url);
void OnWebDocumentTitleChanged(void* ctx, WebviewWnd* sender, Str title);
void OnWebPanelJsNotify(void* ctx, Str method, Str paramsJson);
int FindWebPanelTabByWebView(MainWindow* win, WebviewWnd* wv);
void SyncWebPanelTabFromWebView(MainWindow* win, WebviewWnd* wv, Str url, Str title);
TempStr EscapeJsonTemp(Str s);

TempStr WebPanelDataDirTemp() {
    // Canonical tree under %OneDrive%\SumatraPDF\WebPanel\ (unified backup root).
    return GetPathInAppDataDirTemp(StrL("WebPanel"));
}

static void MigrateWebPanelFile(Str fromRel, Str toRel) {
    TempStr root = WebPanelDataDirTemp();
    TempStr from = path::JoinTemp(root, fromRel);
    TempStr to = path::JoinTemp(root, toRel);
    if (!file::Exists(from) || file::Exists(to)) {
        return;
    }
    dir::CreateAll(path::GetDirTemp(to));
    file::Copy(to, from, false);
    // Keep the old file as a safety copy until the next cleanup; prefer new path.
}

static void MigrateWebPanelDir(Str fromRel, Str toRel) {
    TempStr root = WebPanelDataDirTemp();
    TempStr from = path::JoinTemp(root, fromRel);
    TempStr to = path::JoinTemp(root, toRel);
    if (!dir::Exists(from) || dir::Exists(to)) {
        return;
    }
    dir::CreateAll(path::GetDirTemp(to));
    // Best-effort rename; if locked, leave old path (readers still fall back below).
    MoveFileExW(CWStrTemp(from), CWStrTemp(to), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
}

void EnsureWebPanelDataLayout() {
    TempStr root = WebPanelDataDirTemp();
    dir::CreateAll(path::JoinTemp(root, StrL("tabs")));
    dir::CreateAll(path::JoinTemp(root, StrL("bridge")));
    dir::CreateAll(path::JoinTemp(root, StrL("profile")));
    dir::CreateAll(path::JoinTemp(root, StrL("cache\\favicons")));
    dir::CreateAll(path::JoinTemp(root, StrL("jobs\\pending")));
    dir::CreateAll(path::JoinTemp(root, StrL("jobs\\done")));
    dir::CreateAll(path::JoinTemp(root, StrL("jobs\\failed")));
    // Legacy flat layout → structured (idempotent).
    MigrateWebPanelFile(StrL("tabs.json"), StrL("tabs\\index.json"));
    MigrateWebPanelFile(StrL("pdf-tabs.json"), StrL("tabs\\pdf-map.json"));
    MigrateWebPanelFile(StrL("web-bridge.json"), StrL("bridge\\web-bridge.json"));
    MigrateWebPanelFile(StrL("bridge.log"), StrL("bridge\\bridge.log"));
    MigrateWebPanelDir(StrL("favicons"), StrL("cache\\favicons"));
    MigrateWebPanelDir(StrL("WebView2"), StrL("profile\\WebView2"));
    // Human-readable layout guide for OneDrive backup.
    TempStr readme = path::JoinTemp(root, StrL("README.txt"));
    if (!file::Exists(readme)) {
        file::WriteFile(readme,
                        StrL("SumatraPDF WebPanel data (unified under %OneDrive%\\SumatraPDF\\WebPanel)\r\n"
                             "\r\n"
                             "tabs\\             tab session + per-PDF active-tab map\r\n"
                             "  index.json       open tabs / active tab id\r\n"
                             "  pdf-map.json     bookId → {webTabId,webTabUrl,notebookTabId,notebookTabUrl}\r\n"
                             "bridge\\           CDP / Playwright bridge state\r\n"
                             "  web-bridge.json  live endpoint + current PDF context\r\n"
                             "profile\\          browser profile (cookies / accounts)\r\n"
                             "  WebView2\\        WebView2 user-data folder\r\n"
                             "cache\\            disposable caches\r\n"
                             "  favicons\\        host favicon PNGs\r\n"
                             "jobs\\             automation queue\r\n"
                             "  pending|done|failed\r\n"
                             "bookmarks.txt      AI bookmark list\r\n"));
    }
}

static TempStr PreferNewOrLegacyTemp(Str newRel, Str legacyRel) {
    TempStr root = WebPanelDataDirTemp();
    TempStr neu = path::JoinTemp(root, newRel);
    if (file::Exists(neu) || dir::Exists(neu)) {
        return neu;
    }
    TempStr old = path::JoinTemp(root, legacyRel);
    if (file::Exists(old) || dir::Exists(old)) {
        return old;
    }
    return neu;
}

TempStr BookmarksPathTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("bookmarks.txt"));
}

TempStr TabsPathCanonicalTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("tabs\\index.json"));
}

TempStr TabsPathTemp() {
    return PreferNewOrLegacyTemp(StrL("tabs\\index.json"), StrL("tabs.json"));
}

TempStr PdfTabsMapPathCanonicalTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("tabs\\pdf-map.json"));
}

TempStr PdfTabsMapPathTemp() {
    return PreferNewOrLegacyTemp(StrL("tabs\\pdf-map.json"), StrL("pdf-tabs.json"));
}

TempStr BridgePathCanonicalTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("bridge\\web-bridge.json"));
}

TempStr BridgePathTemp() {
    return PreferNewOrLegacyTemp(StrL("bridge\\web-bridge.json"), StrL("web-bridge.json"));
}

TempStr WebViewProfileDirTemp() {
    return PreferNewOrLegacyTemp(StrL("profile\\WebView2"), StrL("WebView2"));
}

TempStr FaviconsDirTemp() {
    return PreferNewOrLegacyTemp(StrL("cache\\favicons"), StrL("favicons"));
}

TempStr WebPanelJobsPendingTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("jobs\\pending"));
}

TempStr WebPanelJobsDoneTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("jobs\\done"));
}

TempStr WebPanelJobsFailedTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("jobs\\failed"));
}

// host for https://a.b/c → a.b
TempStr HostFromUrlTemp(Str url) {
    if (!url) {
        return {};
    }
    Str rest = url;
    if (str::StartsWithI(rest, StrL("https://"))) {
        rest = Str(rest.s + 8, rest.len - 8);
    } else if (str::StartsWithI(rest, StrL("http://"))) {
        rest = Str(rest.s + 7, rest.len - 7);
    } else {
        return {};
    }
    int end = 0;
    while (end < rest.len && rest.s[end] != '/' && rest.s[end] != '?' && rest.s[end] != '#' && rest.s[end] != ':') {
        end++;
    }
    if (end <= 0) {
        return {};
    }
    return str::DupTemp(Str(rest.s, end));
}

TempStr FaviconCachePathTemp(Str host) {
    if (!host) {
        return {};
    }
    str::Builder name;
    for (int i = 0; i < host.len; i++) {
        char c = host.s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') {
            name.AppendChar(c);
        } else {
            name.AppendChar('_');
        }
    }
    name.Append(StrL(".png"));
    return path::JoinTemp(FaviconsDirTemp(), ToStrTemp(name));
}

// browser-style favicon from disk cache only (never network on UI thread).
// Sized to fit the sidebar header (same band as Library / ToC labels).
constexpr int kPinIconPx = 14;

Pixmap* LoadFaviconPixmap(Str url) {
    int sz = DpiScale(kPinIconPx);
    TempStr host = HostFromUrlTemp(url);
    if (host) {
        TempStr cachePath = FaviconCachePathTemp(host);
        if (cachePath && file::Exists(cachePath)) {
            Str data = file::ReadFile(cachePath);
            if (data) {
                Pixmap* px = PixmapFromData(data);
                str::Free(data);
                if (px) {
                    // VirtIconButton blits at native pixmap size — keep a small copy
                    if (px->width == sz && px->height == sz) {
                        return px;
                    }
                    // draw into an ideal-sized pixmap via VirtImage-style fit:
                    // store original; pin button scales at paint time
                    return px;
                }
            }
        }
    }
    return GetCachedPixmapForSvg(gIconChat, sz, sz);
}

void FetchMissingFavicons(Vec<Str>* urls) {
    if (!urls) {
        return;
    }
    dir::CreateAll(FaviconsDirTemp());
    for (Str& url : *urls) {
        TempStr host = HostFromUrlTemp(url);
        if (!host) {
            continue;
        }
        TempStr cachePath = FaviconCachePathTemp(host);
        if (!cachePath || file::Exists(cachePath)) {
            continue;
        }
        TempStr favUrl = fmt("https://www.google.com/s2/favicons?domain=%s&sz=32", host);
        HttpRsp rsp;
        if (HttpGet(favUrl, &rsp) && IsHttpRspOk(&rsp) && len(rsp.data) > 0) {
            file::WriteFile(cachePath, ToStr(rsp.data));
        }
    }
}

struct FaviconJob {
    MainWindow* win = nullptr;
    Vec<Str>* urls = nullptr;
};

void OnFaviconsFetched(FaviconJob* job) {
    if (!job) {
        return;
    }
    MainWindow* win = job->win;
    if (job->urls) {
        for (Str& u : *job->urls) {
            str::Free(u);
        }
        delete job->urls;
    }
    delete job;
    if (!IsMainWindowValid(win) || !win->hwndWebPanelBox) {
        return;
    }
    RebuildPinStrip(win);
    RelayoutWebPanel(win);
}

void FaviconFetchThread(FaviconJob* job) {
    if (!job) {
        return;
    }
    FetchMissingFavicons(job->urls);
    uitask::Post(MkFunc0(OnFaviconsFetched, job), "WebPanelFaviconsDone");
}

void ScheduleFaviconPrefetch(MainWindow* win) {
    if (!win) {
        return;
    }
    auto* urls = new Vec<Str>();
    for (WebBookmark& b : gBookmarks) {
        if (b.pinned && b.url) {
            TempStr host = HostFromUrlTemp(b.url);
            TempStr cachePath = host ? FaviconCachePathTemp(host) : TempStr{};
            if (cachePath && file::Exists(cachePath)) {
                continue;
            }
            urls->Append(str::Dup(b.url));
        }
    }
    if (len(*urls) == 0) {
        delete urls;
        return;
    }
    auto* job = new FaviconJob{win, urls};
    RunAsync(MkFunc0(FaviconFetchThread, job), "WebPanelFavicons");
}

void DeferredEnsureWebPanelWebView(MainWindow* win) {
    if (!IsMainWindowValid(win) || !win->uiState.webPanelVisible) {
        return;
    }
    EnsureWebPanelWebView(win);
    ScheduleUiUpdate(win);
    ScheduleFaviconPrefetch(win);
}

TempStr ClipboardTextTemp() {
    if (!OpenClipboard(nullptr)) {
        return {};
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    TempStr out = {};
    if (h) {
        auto* w = (WCHAR*)GlobalLock(h);
        if (w) {
            out = ToUtf8Temp(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void FreeBookmarks() {
    for (WebBookmark& b : gBookmarks) {
        str::Free(b.title);
        str::Free(b.url);
    }
    gBookmarks.Reset();
}

void EnsureDefaultBookmarks() {
    bool changed = false;
    for (const DefaultBookmark& def : kDefaultBookmarks) {
        if (FindBookmarkByUrl(def.url) >= 0) {
            continue;
        }
        WebBookmark b;
        b.title = str::Dup(def.title);
        b.url = str::Dup(def.url);
        b.pinned = true;
        gBookmarks.Append(b);
        changed = true;
    }
    if (changed) {
        SaveBookmarks();
    }
}

void LoadBookmarks() {
    FreeBookmarks();
    str::Free(gLastUrl);
    gLastUrl = {};
    gCdpPort = kDefaultCdpPort;

    TempStr path = BookmarksPathTemp();
    Str data = file::ReadFile(path);
    if (!data) {
        EnsureDefaultBookmarks();
        return;
    }
    StrVec lines;
    Split(&lines, data, StrL("\n"), true);
    str::Free(data);
    for (Str line : lines) {
        while (line.len > 0 && (line.s[line.len - 1] == '\r' || line.s[line.len - 1] == ' ')) {
            line.len--;
        }
        if (line.len == 0 || line.s[0] == '#') {
            continue;
        }
        if (str::StartsWith(line, StrL("last="))) {
            TempStr last = str::DupTemp(Str(line.s + 5, line.len - 5));
            if (last && !str::EqI(last, StrL("about:blank")) && !str::StartsWithI(last, StrL("about:"))) {
                gLastUrl = str::Dup(last);
            }
            continue;
        }
        if (str::StartsWith(line, StrL("cdp="))) {
            int port = 0;
            if (str::Parse(Str(line.s + 4, line.len - 4), "%d", &port).s && port > 0) {
                // Migrate pre-9224 profiles; keep a single well-known port for the AI bridge.
                gCdpPort = (port == 9223) ? kDefaultCdpPort : port;
            }
            continue;
        }
        StrVec parts;
        Split(&parts, line, StrL("|"), false);
        if (len(parts) < 2) {
            continue;
        }
        WebBookmark b;
        b.title = str::Dup(parts[0]);
        b.url = str::Dup(parts[1]);
        b.pinned = len(parts) >= 3 && str::Eq(parts[2], StrL("1"));
        if (b.title && b.url) {
            gBookmarks.Append(b);
        } else {
            str::Free(b.title);
            str::Free(b.url);
        }
    }
    EnsureDefaultBookmarks();
}

void SaveBookmarks() {
    TempStr dir = WebPanelDataDirTemp();
    dir::CreateAll(dir);
    str::Builder sb;
    sb.Append("# WebPanel bookmarks: title|url|pinned\n");
    if (gLastUrl) {
        sb.Append(fmt("last=%s\n", gLastUrl));
    }
    sb.Append(fmt("cdp=%d\n", gCdpPort));
    for (WebBookmark& b : gBookmarks) {
        sb.Append(fmt("%s|%s|%d\n", b.title ? b.title : "", b.url ? b.url : "", b.pinned ? 1 : 0));
    }
    file::WriteFile(BookmarksPathTemp(), ToStr(sb));
}

static i64 DirSizeBytes(Str dir) {
    if (!dir || !dir::Exists(dir)) {
        return 0;
    }
    i64 total = 0;
    DirIter di(dir);
    di.recurse = true;
    di.includeFiles = true;
    di.includeDirs = false;
    for (DirIterEntry* e : di) {
        if (e && e->isFile) {
            total += e->size > 0 ? e->size : 0;
        }
    }
    return total;
}

static i64 ProcessWorkingSetBytes(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        return 0;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    i64 bytes = 0;
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
        bytes = (i64)pmc.WorkingSetSize;
    }
    CloseHandle(h);
    return bytes;
}

static i64 WebViewChildWorkingSetBytes() {
    i64 total = 0;
    DWORD myPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == myPid) {
                TempWStr name = pe.szExeFile;
                TempStr nameU = ToUtf8Temp(name);
                if (nameU && (str::ContainsI(nameU, StrL("msedgewebview2")) ||
                              str::ContainsI(nameU, StrL("webview2")))) {
                    total += ProcessWorkingSetBytes(pe.th32ProcessID);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return total;
}

static ULONGLONG FileTimeToU64(const FILETIME& ft) {
    return (((ULONGLONG)ft.dwHighDateTime) << 32) | (ULONGLONG)ft.dwLowDateTime;
}

static void AccumulateWebViewChildCpuTimes(ULONGLONG* kernelOut, ULONGLONG* userOut) {
    if (!kernelOut || !userOut) {
        return;
    }
    DWORD myPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID != myPid) {
                continue;
            }
            TempStr nameU = ToUtf8Temp(pe.szExeFile);
            if (!nameU || !(str::ContainsI(nameU, StrL("msedgewebview2")) ||
                            str::ContainsI(nameU, StrL("webview2")))) {
                continue;
            }
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) {
                h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
            }
            if (!h) {
                continue;
            }
            FILETIME c{}, e{}, k{}, u{};
            if (GetProcessTimes(h, &c, &e, &k, &u)) {
                *kernelOut += FileTimeToU64(k);
                *userOut += FileTimeToU64(u);
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

struct WebPanelCpuSample {
    ULONGLONG sysIdle = 0;
    ULONGLONG sysKernel = 0;
    ULONGLONG sysUser = 0;
    ULONGLONG procKernel = 0;
    ULONGLONG procUser = 0;
    bool valid = false;
};

static WebPanelCpuSample gWebPanelCpuPrev{};
static double gWebPanelCpuPct = -1;

void UpdateWebPanelCpuSample() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return;
    }
    FILETIME c{}, e{}, pk{}, pu{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) {
        return;
    }
    WebPanelCpuSample cur;
    cur.sysIdle = FileTimeToU64(idle);
    cur.sysKernel = FileTimeToU64(kernel);
    cur.sysUser = FileTimeToU64(user);
    cur.procKernel = FileTimeToU64(pk);
    cur.procUser = FileTimeToU64(pu);
    AccumulateWebViewChildCpuTimes(&cur.procKernel, &cur.procUser);
    cur.valid = true;
    if (gWebPanelCpuPrev.valid) {
        ULONGLONG dSys = (cur.sysKernel - gWebPanelCpuPrev.sysKernel) + (cur.sysUser - gWebPanelCpuPrev.sysUser);
        ULONGLONG dProc =
            (cur.procKernel - gWebPanelCpuPrev.procKernel) + (cur.procUser - gWebPanelCpuPrev.procUser);
        if (dSys > 0) {
            gWebPanelCpuPct = (100.0 * (double)dProc) / (double)dSys;
            if (gWebPanelCpuPct < 0) {
                gWebPanelCpuPct = 0;
            }
            if (gWebPanelCpuPct > 999) {
                gWebPanelCpuPct = 999;
            }
        }
    }
    gWebPanelCpuPrev = cur;
}

TempStr WebPanelResourceStatsTemp() {
    UpdateWebPanelCpuSample();
    i64 disk = DirSizeBytes(WebPanelDataDirTemp());
    i64 mem = ProcessWorkingSetBytes(GetCurrentProcessId()) + WebViewChildWorkingSetBytes();
    TempStr cpu = gWebPanelCpuPct < 0 ? StrL("—") : fmt("%.0f%%", gWebPanelCpuPct);
    return fmt(_TRA("调试 · CPU %s · 内存 %s · 磁盘 %s · Tab 现场保留").s, cpu,
               FormatFileSizeShortTransTemp(mem), FormatFileSizeShortTransTemp(disk));
}

TempStr NewWebPanelTabIdTemp() {
    return fmt("t%lld-%d", UnixTimeMsNow(), (int)(GetTickCount() & 0xffff));
}

TempStr TitleFromUrlTemp(Str url) {
    TempStr host = HostFromUrlTemp(url);
    return host ? host : (url ? str::DupTemp(url) : StrL("(blank)"));
}

void SaveWebPanelTabs(MainWindow* win) {
    if (!win) {
        return;
    }
    EnsureWebPanelDataLayout();
    str::Builder sb;
    sb.Append(StrL("{\n  \"activeId\": "));
    Str activeId = {};
    if (win->webPanelActiveTab >= 0 && win->webPanelActiveTab < len(win->webPanelTabs)) {
        activeId = win->webPanelTabs[win->webPanelActiveTab].id;
    }
    sb.Append(EscapeJsonTemp(activeId));
    sb.Append(StrL(",\n  \"tabs\": [\n"));
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        WebPanelTab& t = win->webPanelTabs[i];
        if (i > 0) {
            sb.Append(StrL(",\n"));
        }
        sb.Append(StrL("    {\"id\": "));
        sb.Append(EscapeJsonTemp(t.id));
        sb.Append(StrL(", \"url\": "));
        sb.Append(EscapeJsonTemp(t.url));
        sb.Append(StrL(", \"title\": "));
        sb.Append(EscapeJsonTemp(t.title ? t.title : TitleFromUrlTemp(t.url)));
        sb.Append(StrL("}"));
    }
    sb.Append(StrL("\n  ]\n}\n"));
    file::WriteFile(TabsPathCanonicalTemp(), ToStr(sb));
}

void LoadWebPanelTabs(MainWindow* win) {
    if (!win) {
        return;
    }
    // Keep existing live WebViews if already loaded.
    if (len(win->webPanelTabs) > 0) {
        return;
    }
    Str data = file::ReadFile(TabsPathTemp());
    if (!data) {
        return;
    }
    struct St {
        MainWindow* win = nullptr;
        WebPanelTab cur{};
        bool inTab = false;
        Str activeId;
    } st;
    st.win = win;
    auto onVal = [](St* s, json::Value* v) {
        TempStr p = json::PathFormatTemp(v->path);
        if (!p) {
            return;
        }
        if (str::EqI(p, StrL("/activeId"))) {
            str::ReplaceWithCopy(&s->activeId, v->value);
            return;
        }
        if (!str::ContainsI(p, StrL("/tabs"))) {
            return;
        }
        if (str::EndsWithI(p, StrL("/id"))) {
            if (s->inTab && s->cur.url) {
                if (!s->cur.id) {
                    s->cur.id = str::Dup(NewWebPanelTabIdTemp());
                }
                if (!s->cur.title) {
                    s->cur.title = str::Dup(TitleFromUrlTemp(s->cur.url));
                }
                s->win->webPanelTabs.Append(s->cur);
                s->cur = {};
            }
            s->inTab = true;
            str::ReplaceWithCopy(&s->cur.id, v->value);
        } else if (str::EndsWithI(p, StrL("/url"))) {
            str::ReplaceWithCopy(&s->cur.url, v->value);
        } else if (str::EndsWithI(p, StrL("/title"))) {
            str::ReplaceWithCopy(&s->cur.title, v->value);
        }
    };
    json::Parse(data, MkFunc1<St, json::Value*>(onVal, &st));
    if (st.inTab && st.cur.url) {
        if (!st.cur.id) {
            st.cur.id = str::Dup(NewWebPanelTabIdTemp());
        }
        if (!st.cur.title) {
            st.cur.title = str::Dup(TitleFromUrlTemp(st.cur.url));
        }
        win->webPanelTabs.Append(st.cur);
        st.cur = {};
    }
    str::Free(st.cur.id);
    str::Free(st.cur.url);
    str::Free(st.cur.title);
    str::Free(data);
    win->webPanelActiveTab = -1;
    if (st.activeId) {
        for (int i = 0; i < len(win->webPanelTabs); i++) {
            if (str::Eq(win->webPanelTabs[i].id, st.activeId)) {
                win->webPanelActiveTab = i;
                break;
            }
        }
        str::Free(st.activeId);
    }
}

struct PdfTabBinding {
    Str bookKey; // decimal book id
    Str webTabId;
    Str webTabUrl;
    Str notebookTabId;
    Str notebookTabUrl;
};

static bool IsNotebookLmUrl(Str url) {
    if (!url) {
        return false;
    }
    return str::StartsWithI(url, StrL("https://notebook.google.com")) ||
           str::StartsWithI(url, StrL("http://notebook.google.com")) ||
           str::ContainsI(url, StrL("notebooklm.google")) ||
           str::ContainsI(url, StrL("notebook.google.com"));
}

static void FreePdfTabBinding(PdfTabBinding* b) {
    if (!b) {
        return;
    }
    str::Free(b->bookKey);
    str::Free(b->webTabId);
    str::Free(b->webTabUrl);
    str::Free(b->notebookTabId);
    str::Free(b->notebookTabUrl);
    *b = {};
}

static PdfTabBinding* FindPdfTabBinding(Vec<PdfTabBinding>* all, Str bookKey) {
    if (!all || !bookKey) {
        return nullptr;
    }
    for (PdfTabBinding& b : *all) {
        if (str::Eq(b.bookKey, bookKey)) {
            return &b;
        }
    }
    return nullptr;
}

static Vec<PdfTabBinding> LoadAllPdfTabBindings() {
    Vec<PdfTabBinding> all;
    Str data = file::ReadFile(PdfTabsMapPathTemp());
    if (!data) {
        return all;
    }
    struct St {
        Vec<PdfTabBinding>* all = nullptr;
    } st;
    st.all = &all;
    auto onVal = [](St* s, json::Value* v) {
        TempStr p = json::PathFormatTemp(v->path);
        if (!p || p.s[0] != '/') {
            return;
        }
        // /bookKey or /bookKey/field
        Str rest = Str(p.s + 1, p.len - 1);
        int slash = str::IndexOfChar(rest, '/');
        Str bookKey = slash < 0 ? rest : Str(rest.s, slash);
        Str field = slash < 0 ? Str{} : Str(rest.s + slash + 1, rest.len - slash - 1);
        if (!bookKey) {
            return;
        }
        PdfTabBinding* b = FindPdfTabBinding(s->all, bookKey);
        if (!b) {
            PdfTabBinding nb{};
            nb.bookKey = str::Dup(bookKey);
            s->all->Append(nb);
            b = &s->all->Last();
        }
        if (v->type != json::Type::String || !v->value) {
            return;
        }
        if (!field) {
            // Legacy flat "bookId": "tabId"
            if (!b->webTabId) {
                b->webTabId = str::Dup(v->value);
            }
            return;
        }
        if (str::Eq(field, StrL("webTabId")) || str::Eq(field, StrL("tabId"))) {
            str::ReplaceWithCopy(&b->webTabId, v->value);
        } else if (str::Eq(field, StrL("webTabUrl")) || str::Eq(field, StrL("tabUrl"))) {
            str::ReplaceWithCopy(&b->webTabUrl, v->value);
        } else if (str::Eq(field, StrL("notebookTabId"))) {
            str::ReplaceWithCopy(&b->notebookTabId, v->value);
        } else if (str::Eq(field, StrL("notebookTabUrl"))) {
            str::ReplaceWithCopy(&b->notebookTabUrl, v->value);
        }
    };
    json::Parse(data, MkFunc1<St, json::Value*>(onVal, &st));
    str::Free(data);
    return all;
}

static void SaveAllPdfTabBindings(Vec<PdfTabBinding>& all) {
    EnsureWebPanelDataLayout();
    str::Builder out;
    out.Append(StrL("{\n"));
    bool first = true;
    for (PdfTabBinding& b : all) {
        if (!b.bookKey) {
            continue;
        }
        if (!b.webTabId && !b.webTabUrl && !b.notebookTabId && !b.notebookTabUrl) {
            continue;
        }
        if (!first) {
            out.Append(StrL(",\n"));
        }
        first = false;
        out.Append(fmt("  %s: {\n", EscapeJsonTemp(b.bookKey)));
        bool f2 = true;
        auto field = [&](Str name, Str val) {
            if (!val) {
                return;
            }
            if (!f2) {
                out.Append(StrL(",\n"));
            }
            f2 = false;
            out.Append(fmt("    %s: %s", EscapeJsonTemp(name), EscapeJsonTemp(val)));
        };
        field(StrL("webTabId"), b.webTabId);
        field(StrL("webTabUrl"), b.webTabUrl);
        field(StrL("notebookTabId"), b.notebookTabId);
        field(StrL("notebookTabUrl"), b.notebookTabUrl);
        out.Append(StrL("\n  }"));
    }
    out.Append(StrL("\n}\n"));
    file::WriteFile(PdfTabsMapPathCanonicalTemp(), ToStr(out));
}

static void FreeAllPdfTabBindings(Vec<PdfTabBinding>& all) {
    for (PdfTabBinding& b : all) {
        FreePdfTabBinding(&b);
    }
    all.Reset();
}

static PdfTabBinding LoadPdfTabBindingForBook(i64 bookId) {
    PdfTabBinding empty{};
    if (bookId <= 0) {
        return empty;
    }
    TempStr key = fmt("%lld", bookId);
    Vec<PdfTabBinding> all = LoadAllPdfTabBindings();
    PdfTabBinding* found = FindPdfTabBinding(&all, key);
    PdfTabBinding out{};
    if (found) {
        out.bookKey = str::Dup(found->bookKey);
        out.webTabId = str::Dup(found->webTabId);
        out.webTabUrl = str::Dup(found->webTabUrl);
        out.notebookTabId = str::Dup(found->notebookTabId);
        out.notebookTabUrl = str::Dup(found->notebookTabUrl);
    }
    FreeAllPdfTabBindings(all);
    return out;
}

static void UpsertPdfTabBinding(i64 bookId, Str webTabId, Str webTabUrl, Str notebookTabId, Str notebookTabUrl,
                                bool setWeb, bool setNotebook, bool clearIds, bool clearUrls) {
    if (bookId <= 0) {
        return;
    }
    TempStr key = fmt("%lld", bookId);
    Vec<PdfTabBinding> all = LoadAllPdfTabBindings();
    PdfTabBinding* b = FindPdfTabBinding(&all, key);
    if (!b) {
        PdfTabBinding nb{};
        nb.bookKey = str::Dup(key);
        all.Append(nb);
        b = &all.Last();
    }
    if (clearIds) {
        str::Free(b->webTabId);
        str::Free(b->notebookTabId);
        b->webTabId = {};
        b->notebookTabId = {};
    }
    if (clearUrls) {
        str::Free(b->webTabUrl);
        str::Free(b->notebookTabUrl);
        b->webTabUrl = {};
        b->notebookTabUrl = {};
    }
    if (setWeb) {
        if (webTabId) {
            str::ReplaceWithCopy(&b->webTabId, webTabId);
        }
        if (webTabUrl) {
            str::ReplaceWithCopy(&b->webTabUrl, webTabUrl);
        }
    }
    if (setNotebook) {
        if (notebookTabId) {
            str::ReplaceWithCopy(&b->notebookTabId, notebookTabId);
        }
        if (notebookTabUrl) {
            str::ReplaceWithCopy(&b->notebookTabUrl, notebookTabUrl);
        }
    }
    SaveAllPdfTabBindings(all);
    FreeAllPdfTabBindings(all);
}

int FindWebPanelTabById(MainWindow* win, Str id) {
    if (!win || !id) {
        return -1;
    }
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        if (str::Eq(win->webPanelTabs[i].id, id)) {
            return i;
        }
    }
    return -1;
}

// Activate by stable tab id; if the live tab was closed, reopen from saved URL keeping the same id.
bool ActivateOrRecreateWebPanelTab(MainWindow* win, Str tabId, Str tabUrl, Str titleFallback, bool rememberPdf) {
    if (!win) {
        return false;
    }
    if (tabId) {
        int idx = FindWebPanelTabById(win, tabId);
        if (idx >= 0) {
            ActivateWebPanelTabByIndex(win, idx, rememberPdf);
            return true;
        }
    }
    if (tabUrl && tabUrl.len > 0) {
        CreateNewWebPanelTab(win, tabUrl, titleFallback && titleFallback.len > 0 ? titleFallback : TitleFromUrlTemp(tabUrl),
                             tabId);
        return true;
    }
    return false;
}

void RememberPdfActiveTab(MainWindow* win) {
    if (!win || win->webPanelActiveTab < 0 || win->webPanelActiveTab >= len(win->webPanelTabs)) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath || !LibraryIsAvailable()) {
        return;
    }
    LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
    if (!book) {
        return;
    }
    WebPanelTab& t = win->webPanelTabs[win->webPanelActiveTab];
    // Always remember the currently displayed WebView tab (by stable id + full url).
    UpsertPdfTabBinding(book->id, t.id, t.url, {}, {}, true, false, false, false);
    // Separately remember NotebookLM slot when this tab is a NotebookLM page.
    if (IsNotebookLmUrl(t.url)) {
        UpsertPdfTabBinding(book->id, {}, {}, t.id, t.url, false, true, false, false);
    }
    DeleteLibraryBook(book);
    SaveWebPanelTabs(win);
}

void RestorePdfActiveTab(MainWindow* win) {
    if (!win || !LibraryIsAvailable()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
    if (!book) {
        return;
    }
    PdfTabBinding bind = LoadPdfTabBindingForBook(book->id);
    DeleteLibraryBook(book);
    // Prefer stable tab id; fall back to saved URL if the tab was closed from the menu.
    ActivateOrRecreateWebPanelTab(win, bind.webTabId, bind.webTabUrl, TitleFromUrlTemp(bind.webTabUrl), false);
    FreePdfTabBinding(&bind);
}

void RestorePdfNotebookLmTab(MainWindow* win) {
    if (!win || !LibraryIsAvailable()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        // No PDF → open generic NotebookLM home.
        ActivateOrRecreateWebPanelTab(win, {}, StrL("https://notebook.google.com/"), StrL("NotebookLM"), false);
        return;
    }
    LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
    if (!book) {
        ActivateOrRecreateWebPanelTab(win, {}, StrL("https://notebook.google.com/"), StrL("NotebookLM"), false);
        return;
    }
    PdfTabBinding bind = LoadPdfTabBindingForBook(book->id);
    DeleteLibraryBook(book);
    bool ok = ActivateOrRecreateWebPanelTab(win, bind.notebookTabId, bind.notebookTabUrl,
                                            StrL("NotebookLM"), true);
    if (!ok) {
        ActivateOrRecreateWebPanelTab(win, {}, StrL("https://notebook.google.com/"), StrL("NotebookLM"), true);
    }
    FreePdfTabBinding(&bind);
}

TempStr EscapeJsonTemp(Str s) {
    str::Builder out;
    out.AppendChar('"');
    if (s) {
        for (int i = 0; i < s.len; i++) {
            char c = s.s[i];
            if (c == '"' || c == '\\') {
                out.AppendChar('\\');
                out.AppendChar(c);
            } else if (c == '\n') {
                out.Append(StrL("\\n"));
            } else if (c == '\r') {
                out.Append(StrL("\\r"));
            } else if ((u8)c < 0x20) {
                out.Append(fmt("\\u%04x", (unsigned)(u8)c));
            } else {
                out.AppendChar(c);
            }
        }
    }
    out.AppendChar('"');
    return ToStrTemp(out);
}

void WriteBridgeJson(MainWindow* win) {
    EnsureWebPanelDataLayout();
    TempStr pdfPath = {};
    TempStr fileName = {};
    if (win && win->CurrentTab() && win->CurrentTab()->filePath) {
        pdfPath = win->CurrentTab()->filePath;
        fileName = path::GetBaseNameTemp(pdfPath);
    }
    TempStr url = win && win->webPanelCurrentUrl ? win->webPanelCurrentUrl : gLastUrl;
    TempStr profile = WebViewProfileDirTemp();
    int port = win && win->webPanelCdpPort > 0 ? win->webPanelCdpPort : gCdpPort;
    TempStr json = fmt(
        "{\n"
        "  \"cdpPort\": %d,\n"
        "  \"cdpEndpoint\": \"http://127.0.0.1:%d\",\n"
        "  \"userDataDir\": %s,\n"
        "  \"url\": %s,\n"
        "  \"pdfPath\": %s,\n"
        "  \"fileName\": %s\n"
        "}\n",
        port, port, EscapeJsonTemp(profile), EscapeJsonTemp(url), EscapeJsonTemp(pdfPath), EscapeJsonTemp(fileName));
    file::WriteFile(BridgePathCanonicalTemp(), json);
}

void LayoutWebPanelBox(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox || !win->webPanelLayout) {
        return;
    }
    Rect rc = HwndClientRect(win->hwndWebPanelBox);
    LayoutTreeToSize(win->hwndWebPanelBox, win->webPanelLayout, {rc.dx, rc.dy}, &win->webPanelRoot);
    ShowActiveWebPanelTab(win);
}

void ShowActiveWebPanelTab(MainWindow* win) {
    if (!win || !win->webPanelWebViewSlot) {
        return;
    }
    Rect wr = win->webPanelWebViewSlot->lastBounds;
    if (wr.dx < 1) {
        wr.dx = 1;
    }
    if (wr.dy < 1) {
        wr.dy = 1;
    }
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        WebviewWnd* wv = win->webPanelTabs[i].wv;
        if (!wv || !wv->hwnd) {
            continue;
        }
        bool active = (i == win->webPanelActiveTab);
        if (active) {
            // stay strictly in the slot below the header so pin clicks work
            MoveWindow(wv->hwnd, wr.x, wr.y, wr.dx, wr.dy, TRUE);
            wv->SetIsVisible(true);
            wv->SetControllerVisible(true);
            wv->UpdateWebviewSize();
            win->webPanelWebView = wv;
        } else {
            wv->SetControllerVisible(false);
            wv->SetIsVisible(false);
            ShowWindow(wv->hwnd, SW_HIDE);
        }
    }
}

int FindWebPanelTab(MainWindow* win, Str url) {
    if (!win || !url) {
        return -1;
    }
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        if (win->webPanelTabs[i].url && str::EqI(win->webPanelTabs[i].url, url)) {
            return i;
        }
    }
    return -1;
}

WebviewWnd* CreateWebPanelTabWebView(MainWindow* win, Str url) {
    if (!win || !url || !HasWebView()) {
        return nullptr;
    }
    EnsureWebPanelDataLayout();
    dir::CreateAll(WebViewProfileDirTemp());

    int port = gCdpPort > 0 ? gCdpPort : kDefaultCdpPort;
    win->webPanelCdpPort = port;

    auto* webView = new WebviewWnd();
    webView->events.ctx = win;
    webView->events.navigationStarting = OnWebNavStarting;
    webView->events.navigationCompleted = OnWebNavCompleted;
    webView->events.sourceChanged = OnWebSourceChanged;
    webView->events.documentTitleChanged = OnWebDocumentTitleChanged;
    webView->events.jsNotify = OnWebPanelJsNotify;
    webView->dataDir = str::Dup(WebViewProfileDirTemp());
    webView->useDedicatedEnvironment = true;
    webView->enableDevTools = true;
    webView->emulateMobile = true;
    webView->mobileDeviceWidth = 0;
    webView->mobileDeviceHeight = 0;
    webView->mobileDeviceScale = 1.0f;
    webView->userAgent = str::Dup(kMobileUserAgent);
    webView->dedicatedBrowserArgs = str::Dup(fmt("--remote-debugging-port=%d", port));
    webView->allowClipboardRead = true;
    webView->desiredVisible = false;
    webView->AddInitScript(kMobileViewportScript);

    CreateWebViewArgs wvArgs;
    wvArgs.parent = win->hwndWebPanelBox;
    wvArgs.pos = Rect(0, 0, 1, 1);
    webView->Create(wvArgs);
    if (!webView->hwnd) {
        delete webView;
        return nullptr;
    }
    ShowWindow(webView->hwnd, SW_HIDE);
    webView->Navigate(url);
    return webView;
}

void ActivateWebPanelTabByIndex(MainWindow* win, int idx, bool rememberPdf) {
    if (!win || idx < 0 || idx >= len(win->webPanelTabs)) {
        return;
    }
    WebPanelTab& t = win->webPanelTabs[idx];
    if (!t.wv && t.url) {
        t.wv = CreateWebPanelTabWebView(win, t.url);
        if (!t.wv) {
            return;
        }
    }
    win->webPanelActiveTab = idx;
    if (t.url) {
        str::ReplaceWithCopy(&win->webPanelCurrentUrl, t.url);
        str::ReplaceWithCopy(&gLastUrl, t.url);
    }
    win->webPanelWebView = t.wv;
    win->webPanelWebViewReady = t.wv != nullptr;
    ShowActiveWebPanelTab(win);
    SaveWebPanelTabs(win);
    WriteBridgeJson(win);
    RelayoutWebPanel(win);
    if (rememberPdf) {
        RememberPdfActiveTab(win);
    }
}

void CreateNewWebPanelTab(MainWindow* win, Str url, Str title, Str forcedId) {
    if (!win || !url) {
        return;
    }
    // If forcedId already exists as a live tab, just activate it (do not duplicate).
    if (forcedId) {
        int existing = FindWebPanelTabById(win, forcedId);
        if (existing >= 0) {
            ActivateWebPanelTabByIndex(win, existing, true);
            return;
        }
    }
    WebviewWnd* wv = CreateWebPanelTabWebView(win, url);
    if (!wv) {
        return;
    }
    WebPanelTab tab;
    tab.id = str::Dup(forcedId && forcedId.len > 0 ? forcedId : NewWebPanelTabIdTemp());
    tab.url = str::Dup(url);
    tab.title = str::Dup(title && title.len > 0 ? title : TitleFromUrlTemp(url));
    tab.wv = wv;
    win->webPanelTabs.Append(tab);
    ActivateWebPanelTabByIndex(win, len(win->webPanelTabs) - 1, true);
}

void CloseWebPanelTabAt(MainWindow* win, int idx) {
    if (!win || idx < 0 || idx >= len(win->webPanelTabs)) {
        return;
    }
    // Closing a live tab does NOT clear per-PDF bindings (id/url stay so restore can reopen).
    WebPanelTab& t = win->webPanelTabs[idx];
    delete t.wv;
    str::Free(t.id);
    str::Free(t.url);
    str::Free(t.title);
    win->webPanelTabs.RemoveAt(idx);
    if (win->webPanelActiveTab == idx) {
        win->webPanelActiveTab = -1;
        win->webPanelWebView = nullptr;
        win->webPanelWebViewReady = false;
        if (len(win->webPanelTabs) > 0) {
            int next = idx < len(win->webPanelTabs) ? idx : len(win->webPanelTabs) - 1;
            ActivateWebPanelTabByIndex(win, next, true);
        }
    } else if (win->webPanelActiveTab > idx) {
        win->webPanelActiveTab--;
    }
    SaveWebPanelTabs(win);
    ShowActiveWebPanelTab(win);
    RelayoutWebPanel(win);
}

void CloseAllWebPanelTabs(MainWindow* win) {
    if (!win) {
        return;
    }
    for (int i = len(win->webPanelTabs) - 1; i >= 0; i--) {
        WebPanelTab& t = win->webPanelTabs[i];
        delete t.wv;
        str::Free(t.id);
        str::Free(t.url);
        str::Free(t.title);
    }
    win->webPanelTabs.Clear();
    win->webPanelActiveTab = -1;
    win->webPanelWebView = nullptr;
    win->webPanelWebViewReady = false;
    SaveWebPanelTabs(win);
    ShowActiveWebPanelTab(win);
    RelayoutWebPanel(win);
}

void ActivateWebPanelTab(MainWindow* win, Str url) {
    if (!win || !url) {
        return;
    }
    int idx = FindWebPanelTab(win, url);
    // Reuse NotebookLM tab for automation (Add PDF) — avoid white-screen duplicates.
    if (idx < 0 && (str::ContainsI(url, StrL("notebook.google.com")) ||
                    str::ContainsI(url, StrL("notebooklm.google")))) {
        for (int i = 0; i < len(win->webPanelTabs); i++) {
            Str u = win->webPanelTabs[i].url;
            if (u && (str::ContainsI(u, StrL("notebook.google.com")) ||
                      str::ContainsI(u, StrL("notebooklm.google")) || str::EqI(u, StrL("about:blank")))) {
                idx = i;
                str::ReplaceWithCopy(&win->webPanelTabs[i].url, url);
                if (win->webPanelTabs[i].wv) {
                    win->webPanelTabs[i].wv->Navigate(url);
                }
                break;
            }
        }
    }
    if (idx >= 0) {
        ActivateWebPanelTabByIndex(win, idx, true);
        return;
    }
    CreateNewWebPanelTab(win, url, TitleFromUrlTemp(url));
}

void NavigateWebPanel(MainWindow* win, Str url) {
    ActivateWebPanelTab(win, url);
}

void OnWebPanelRefresh(MainWindow* win) {
    if (!win || !win->webPanelWebView) {
        return;
    }
    win->webPanelWebView->Reload();
}

void OnFocusCurrentPdfNotebookLm(MainWindow* win) {
    if (!win) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    i64 bookId = 0;
    LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
    if (book) {
        bookId = book->id;
        DeleteLibraryBook(book);
    }
    TempStr title = path::GetBaseNameTemp(tab->filePath);
    // Manual 1:1 focus — select only this PDF's source and open Chat. No auto-jump on PDF switch.
    WebPanelSelectNotebookLmSource(win, bookId, tab->filePath, title);
}

constexpr int kWebPanelMinDx = 150;
constexpr int kWebPanelMinDocDx = 200;

void OnWebPanelSplitterMove(VirtSplitter::MoveEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->GetHwnd());
    if (!win) {
        return;
    }
    Point pcur = HwndGetCursorPos(win->hwndFrame);
    Rect rFrame = HwndClientRect(win->hwndFrame);
    // Web panel is on the right: width = frameRight - cursorX
    int dx = rFrame.dx - pcur.x;
    // Allow nearly full-window web view; only keep a slim PDF canvas
    int maxDx = std::max(kWebPanelMinDx, rFrame.dx - kWebPanelMinDocDx);
    if (dx < kWebPanelMinDx || dx > maxDx) {
        ev->resizeAllowed = false;
        return;
    }
    if (ev->queryOnly) {
        return;
    }
    win->webPanelDx = dx;
    win->aiChatDx = dx;
    gGlobalPrefs->aiChatSidebarDx = dx;
    if (ev->finishedDragging) {
        SaveSettings();
        // force RelayoutFrame even if previous snapshot matched
        win->uiState.layout = {};
        ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars);
    }
}

int FindBookmarkByUrl(Str url) {
    if (!url) {
        return -1;
    }
    for (int i = 0; i < len(gBookmarks); i++) {
        if (str::Eq(gBookmarks[i].url, url)) {
            return i;
        }
    }
    return -1;
}

void AddOrUpdateBookmark(MainWindow* win, Str title, Str url, bool pinned) {
    if (!url || !str::StartsWithI(url, StrL("http"))) {
        return;
    }
    int idx = FindBookmarkByUrl(url);
    if (idx >= 0) {
        if (title && !str::Eq(gBookmarks[idx].title, title)) {
            str::ReplaceWithCopy(&gBookmarks[idx].title, title);
        }
        if (pinned) {
            gBookmarks[idx].pinned = true;
        }
    } else {
        WebBookmark b;
        b.title = str::Dup(title && title.len > 0 ? title : url);
        b.url = str::Dup(url);
        b.pinned = pinned;
        gBookmarks.Append(b);
    }
    SaveBookmarks();
    RebuildPinStrip(win);
    RelayoutWebPanel(win);
}

void DeleteBookmarkAt(MainWindow* win, int idx) {
    if (idx < 0 || idx >= len(gBookmarks)) {
        return;
    }
    Str url = gBookmarks[idx].url;
    if (win) {
        int tabIdx = FindWebPanelTab(win, url);
        if (tabIdx >= 0) {
            delete win->webPanelTabs[tabIdx].wv;
            str::Free(win->webPanelTabs[tabIdx].url);
            win->webPanelTabs.RemoveAt(tabIdx);
            if (win->webPanelActiveTab == tabIdx) {
                win->webPanelActiveTab = -1;
                win->webPanelWebView = nullptr;
                win->webPanelWebViewReady = false;
            } else if (win->webPanelActiveTab > tabIdx) {
                win->webPanelActiveTab--;
            }
        }
    }
    str::Free(gBookmarks[idx].title);
    str::Free(gBookmarks[idx].url);
    gBookmarks.RemoveAt(idx);
    EnsureDefaultBookmarks();
    SaveBookmarks();
    RebuildPinStrip(win);
    RelayoutWebPanel(win);
    if (win && win->webPanelActiveTab < 0 && len(gBookmarks) > 0) {
        ActivateWebPanelTab(win, gBookmarks[0].url);
    }
}

void TogglePinBookmarkAt(MainWindow* win, int idx) {
    if (idx < 0 || idx >= len(gBookmarks)) {
        return;
    }
    gBookmarks[idx].pinned = !gBookmarks[idx].pinned;
    SaveBookmarks();
    RebuildPinStrip(win);
    RelayoutWebPanel(win);
}

struct PinClickCtx {
    MainWindow* win = nullptr;
    int idx = 0;
};

Vec<PinClickCtx*> gPinClickCtxs;

void FreePinClickCtxs() {
    for (PinClickCtx* c : gPinClickCtxs) {
        delete c;
    }
    gPinClickCtxs.Reset();
}

// Horizontal pin strip: clips overflow and scrolls with the mouse wheel.
struct VirtHScrollStrip : VirtCtrl {
    int scrollX = 0;
    int contentDx = 0;
    int gap = 0;

    VirtHScrollStrip() {
        flags |= vwfClipChildren;
        onMouseWheel = MkMethod1<VirtHScrollStrip, VirtMouseEvent*, &VirtHScrollStrip::OnWheel>(this);
        gap = DpiScale(2);
    }

    Size GetIdealSize() override {
        int h = DpiScale(kPinIconPx + 4);
        int w = 0;
        for (VirtCtrl* c : children) {
            Size s = c->GetIdealSize();
            w += s.dx;
            if (h < s.dy) {
                h = s.dy;
            }
        }
        if (len(children) > 1) {
            w += gap * (len(children) - 1);
        }
        contentDx = w;
        return {w, h};
    }

    int MinIntrinsicWidth(int) override {
        // take whatever flex width the header HBox assigns; we scroll inside
        return 0;
    }

    int MinIntrinsicHeight(int) override {
        return GetIdealSize().dy;
    }

    Size Layout(Constraints bc) override {
        Size ideal = GetIdealSize();
        int dx = bc.max.dx < Inf ? bc.max.dx : ideal.dx;
        return bc.Constrain({dx, ideal.dy});
    }

    void ClampScroll() {
        int view = bounds.dx;
        int maxX = contentDx - view;
        if (maxX < 0) {
            maxX = 0;
        }
        if (scrollX < 0) {
            scrollX = 0;
        }
        if (scrollX > maxX) {
            scrollX = maxX;
        }
    }

    void LayoutChildren() {
        GetIdealSize(); // refresh contentDx
        ClampScroll();
        Rect r = lastBounds;
        int x = r.x - scrollX;
        for (VirtCtrl* c : children) {
            Size s = c->GetIdealSize();
            int y = r.y + (r.dy - s.dy) / 2;
            c->SetBounds({x, y, s.dx, s.dy});
            x += s.dx + gap;
        }
    }

    void SetBounds(Rect r) override {
        VirtCtrl::SetBounds(r);
        LayoutChildren();
    }

    void OnWheel(VirtMouseEvent* ev) {
        if (!ev) {
            return;
        }
        scrollX -= ev->wheelDelta / 4;
        LayoutChildren();
        Invalidate();
        ev->didHandle = true;
    }
};

// Favicon pin that scales the image to the header and shows a hover ✕ to unpin/delete.
struct VirtPinBtn : VirtCtrl {
    Pixmap* pixmap = nullptr; // owned if ownsPixmap
    bool ownsPixmap = false;
    MainWindow* win = nullptr;
    int bookmarkIdx = -1;
    bool hoverClose = false;
    bool selected = false;

    VirtPinBtn() {
        cursor = CursorId::Hand;
        onMouseEnter = MkMethod0<VirtPinBtn, &VirtPinBtn::OnEnter>(this);
        onMouseLeave = MkMethod0<VirtPinBtn, &VirtPinBtn::OnLeave>(this);
        onMouseMove = MkMethod1<VirtPinBtn, VirtMouseEvent*, &VirtPinBtn::OnMove>(this);
        onClick = MkMethod1<VirtPinBtn, VirtMouseEvent*, &VirtPinBtn::OnPinClick>(this);
    }

    ~VirtPinBtn() override {
        if (ownsPixmap) {
            FreePixmap(pixmap);
            pixmap = nullptr;
        }
    }

    Size GetIdealSize() override {
        int sz = DpiScale(kPinIconPx + 4);
        return {sz, sz};
    }

    Rect CloseRectLocal() const {
        int c = DpiScale(10);
        return {bounds.dx - c, 0, c, c};
    }

    void Paint(VirtPaintCtx& ctx) override {
        if (selected) {
            ctx.gfx->FillRect(ctx.bounds, MkGray(0xd0));
        } else if (HasFlag(vwfHovered)) {
            ctx.gfx->FillRect(ctx.bounds, MkGray(0xe8));
        }
        int icon = DpiScale(kPinIconPx);
        Rect dst = {ctx.bounds.x + (ctx.bounds.dx - icon) / 2, ctx.bounds.y + (ctx.bounds.dy - icon) / 2, icon, icon};
        if (pixmap) {
            ctx.gfx->DrawPixmap(pixmap, dst);
        }
        if (HasFlag(vwfHovered)) {
            Rect cr = CloseRectLocal();
            cr.Offset(ctx.bounds.x, ctx.bounds.y);
            Color circle = MkRgba(0xc0, 0x40, 0x40, 0xff);
            ctx.gfx->FillEllipse(cr, circle, 230);
            Color xcol = MkRgba(0xff, 0xff, 0xff, 0xff);
            int pad = cr.dx / 3;
            ctx.gfx->DrawLineAA({cr.x + pad, cr.y + pad}, {cr.x + cr.dx - pad, cr.y + cr.dy - pad}, xcol, 1.5f);
            ctx.gfx->DrawLineAA({cr.x + cr.dx - pad, cr.y + pad}, {cr.x + pad, cr.y + cr.dy - pad}, xcol, 1.5f);
        }
    }

    void OnEnter() {
        Invalidate();
    }
    void OnLeave() {
        hoverClose = false;
        Invalidate();
    }
    void OnMove(VirtMouseEvent* ev) {
        if (!ev) {
            return;
        }
        bool onClose = CloseRectLocal().Contains(ev->pt);
        if (onClose != hoverClose) {
            hoverClose = onClose;
            Invalidate();
        }
    }
    void OnPinClick(VirtMouseEvent* ev) {
        if (!ev || !win) {
            return;
        }
        if (CloseRectLocal().Contains(ev->pt)) {
            DeleteBookmarkAt(win, bookmarkIdx);
            ev->didHandle = true;
            return;
        }
        if (bookmarkIdx >= 0 && bookmarkIdx < len(gBookmarks)) {
            ActivateWebPanelTab(win, gBookmarks[bookmarkIdx].url);
        }
        ev->didHandle = true;
    }
};

void OnPinnedBookmarkClick(PinClickCtx* ctx) {
    if (!ctx || !ctx->win) {
        return;
    }
    if (ctx->idx < 0 || ctx->idx >= len(gBookmarks)) {
        return;
    }
    ActivateWebPanelTab(ctx->win, gBookmarks[ctx->idx].url);
}

void RebuildPinStrip(MainWindow* win) {
    // Pin favicon strip removed — bookmarks open from the menu only.
    (void)win;
}

void ShowBookmarksMenu(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    constexpr UINT kOpenBase = 1000;
    constexpr UINT kAddCurrent = 10;
    constexpr UINT kAddClipboard = 11;

    for (int i = 0; i < len(gBookmarks); i++) {
        TempWStr name = ToWStrTemp(gBookmarks[i].title ? gBookmarks[i].title : gBookmarks[i].url);
        AppendMenuW(menu, MF_STRING, kOpenBase + i, name.s);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kAddCurrent, CWStrTemp(_TRA("添加当前页为书签")));
    AppendMenuW(menu, MF_STRING, kAddClipboard, CWStrTemp(_TRA("从剪贴板添加 URL")));

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, win->hwndWebPanelBox, nullptr);
    DestroyMenu(menu);
    if (cmd == 0) {
        return;
    }
    if (cmd == (int)kAddCurrent) {
        if (win->webPanelCurrentUrl) {
            AddOrUpdateBookmark(win, win->webPanelCurrentUrl, win->webPanelCurrentUrl, false);
        }
        return;
    }
    if (cmd == (int)kAddClipboard) {
        TempStr clip = ClipboardTextTemp();
        while (clip.len > 0 && (clip.s[0] == ' ' || clip.s[0] == '\t' || clip.s[0] == '\r' || clip.s[0] == '\n')) {
            clip.s++;
            clip.len--;
        }
        while (clip.len > 0 &&
               (clip.s[clip.len - 1] == ' ' || clip.s[clip.len - 1] == '\t' || clip.s[clip.len - 1] == '\r' ||
                clip.s[clip.len - 1] == '\n')) {
            clip.len--;
        }
        if (clip.len > 0 && str::StartsWithI(clip, StrL("http"))) {
            AddOrUpdateBookmark(win, clip, clip, false);
            CreateNewWebPanelTab(win, clip, TitleFromUrlTemp(clip));
        }
        return;
    }
    if (cmd >= (int)kOpenBase && cmd < (int)kOpenBase + len(gBookmarks)) {
        // Bookmark click always opens a NEW tab (does not reuse / navigate existing).
        WebBookmark& b = gBookmarks[cmd - kOpenBase];
        CreateNewWebPanelTab(win, b.url, b.title);
    }
}

void OnBookmarksButton(MainWindow* win) {
    ShowBookmarksMenu(win);
}

// Custom tabs popup: left-click activates, middle-click closes (no ✕ — matches browser middle-click close).
struct TabsPopupState {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    int hover = -100; // -2=close-all, >=0=tab index
    int rowH = 0;
    int padX = 0;
    int nTabs = 0;
    int statsH = 0;
    int closeAllY0 = 0;
    int tabsY0 = 0;
    bool hasCloseAll = false;
    Str stats;
    Str emptyHint;
    Str closeAllLabel;
    Vec<Str> titles;
    Vec<bool> activeFlags;
};

static TabsPopupState* gTabsPopup = nullptr;
static ATOM gTabsPopupAtom = 0;

static void DestroyTabsPopup() {
    if (!gTabsPopup) {
        return;
    }
    TabsPopupState* st = gTabsPopup;
    gTabsPopup = nullptr;
    if (st->hwnd) {
        DestroyWindow(st->hwnd);
        st->hwnd = nullptr;
    }
    str::Free(st->stats);
    str::Free(st->emptyHint);
    str::Free(st->closeAllLabel);
    for (Str& t : st->titles) {
        str::Free(t);
    }
    delete st;
}

static int TabsPopupHitRow(TabsPopupState* st, int y) {
    if (!st) {
        return -100;
    }
    if (st->hasCloseAll && y >= st->closeAllY0 && y < st->closeAllY0 + st->rowH) {
        return -2;
    }
    if (y < st->tabsY0) {
        return -100;
    }
    int idx = (y - st->tabsY0) / st->rowH;
    if (idx < 0 || idx >= st->nTabs) {
        return -100;
    }
    return idx;
}

static LRESULT CALLBACK TabsPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TabsPopupState* st = gTabsPopup;
    if (!st || st->hwnd != hwnd) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                DestroyTabsPopup();
            }
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hit = TabsPopupHitRow(st, pt.y);
            if (hit != st->hover) {
                st->hover = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hit = TabsPopupHitRow(st, pt.y);
            MainWindow* win = st->win;
            if (hit == -2) {
                DestroyTabsPopup();
                CloseAllWebPanelTabs(win);
                return 0;
            }
            if (hit >= 0) {
                DestroyTabsPopup();
                ActivateWebPanelTabByIndex(win, hit, true);
                return 0;
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hit = TabsPopupHitRow(st, pt.y);
            if (hit >= 0) {
                MainWindow* win = st->win;
                DestroyTabsPopup();
                CloseWebPanelTabAt(win, hit);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyTabsPopup();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Color bg = ThemeControlBackgroundColor();
            Color fg = ThemeWindowTextColor();
            Color muted = ThemeWindowTextDisabledColor();
            Color hot = ThemeHotBackgroundColor();
            Color edge = ThemeEdgeColor();
            HBRUSH brBg = CreateSolidBrush(bg);
            FillRect(hdc, &rc, brBg);
            DeleteObject(brBg);

            SetBkMode(hdc, TRANSPARENT);
            // Use normal menu font (sidebar label font is bold by design).
            PlatformFont* pfont = GetAppMenuFont();
            HFONT font = pfont ? pfont->GetHFont() : nullptr;
            HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : nullptr;

            auto drawTextRow = [&](int y, Str text, Color col, bool highlight, bool activeDot) {
                RECT row{0, y, rc.right, y + st->rowH};
                if (highlight) {
                    HBRUSH brHot = CreateSolidBrush(hot);
                    FillRect(hdc, &row, brHot);
                    DeleteObject(brHot);
                }
                SetTextColor(hdc, col);
                int left = st->padX;
                if (activeDot) {
                    const WCHAR* dot = L"\u25CF ";
                    TextOutW(hdc, left, y + (st->rowH - 16) / 2, dot, 2);
                    SIZE sz{};
                    GetTextExtentPoint32W(hdc, dot, 2, &sz);
                    left += sz.cx;
                }
                RECT textRc{left, y, rc.right - st->padX, y + st->rowH};
                TempWStr wText = ToWStrTemp(text);
                DrawTextW(hdc, wText.s, wText.len, &textRc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            };

            drawTextRow(0, st->stats, muted, false, false);
            HPEN pen = CreatePen(PS_SOLID, 1, edge);
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            int sepY = st->statsH;
            MoveToEx(hdc, st->padX, sepY, nullptr);
            LineTo(hdc, rc.right - st->padX, sepY);

            if (st->hasCloseAll) {
                drawTextRow(st->closeAllY0, st->closeAllLabel, fg, st->hover == -2, false);
                int sep2 = st->closeAllY0 + st->rowH;
                MoveToEx(hdc, st->padX, sep2, nullptr);
                LineTo(hdc, rc.right - st->padX, sep2);
            }

            if (st->nTabs == 0) {
                drawTextRow(st->tabsY0, st->emptyHint, muted, false, false);
            } else {
                for (int i = 0; i < st->nTabs; i++) {
                    drawTextRow(st->tabsY0 + i * st->rowH, st->titles[i], fg, st->hover == i, st->activeFlags[i]);
                }
            }

            SelectObject(hdc, oldPen);
            DeleteObject(pen);
            if (oldFont) {
                SelectObject(hdc, oldFont);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (gTabsPopup && gTabsPopup->hwnd == hwnd) {
                gTabsPopup->hwnd = nullptr;
                DestroyTabsPopup();
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowTabsMenu(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    DestroyTabsPopup();
    EnsureWebPanelDataLayout();
    LoadWebPanelTabs(win);
    // Refresh live URL/title from WebViews before listing (SPA may have pushState'd).
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        WebPanelTab& t = win->webPanelTabs[i];
        if (!t.wv) {
            continue;
        }
        TempStr url = t.wv->GetSourceTemp();
        TempStr title = t.wv->GetDocumentTitleTemp();
        SyncWebPanelTabFromWebView(win, t.wv, url, title);
    }
    UpdateWebPanelCpuSample();
    UpdateWebPanelCpuSample();

    auto* st = new TabsPopupState();
    st->win = win;
    st->nTabs = len(win->webPanelTabs);
    st->stats = str::Dup(WebPanelResourceStatsTemp());
    st->emptyHint = str::Dup(_TRA("（暂无 Tab — 点书签新建）"));
    st->closeAllLabel = str::Dup(_TRA("关闭所有标签页"));
    st->hasCloseAll = st->nTabs > 0;
    st->rowH = DpiScale(26);
    st->padX = DpiScale(10);
    st->statsH = st->rowH;
    st->closeAllY0 = st->statsH + 1;
    st->tabsY0 = st->hasCloseAll ? (st->closeAllY0 + st->rowH + 1) : (st->statsH + 1);
    for (int i = 0; i < st->nTabs; i++) {
        WebPanelTab& t = win->webPanelTabs[i];
        // Prefer live document.title; fall back to stored title / host.
        TempStr liveTitle = t.wv ? t.wv->GetDocumentTitleTemp() : TempStr{};
        Str shown = (liveTitle && liveTitle.len > 0 && !str::StartsWithI(liveTitle, StrL("http")))
                        ? Str(liveTitle)
                        : (t.title ? t.title : TitleFromUrlTemp(t.url));
        st->titles.Append(str::Dup(shown));
        st->activeFlags.Append(i == win->webPanelActiveTab);
    }

    if (!gTabsPopupAtom) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TabsPopupWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SUMATRA_WEBPANEL_TABS_MENU";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DROPSHADOW;
        gTabsPopupAtom = RegisterClassW(&wc);
    }

    int height = st->statsH + 1 + (st->hasCloseAll ? st->rowH + 1 : 0) +
                 (st->nTabs > 0 ? st->nTabs * st->rowH : st->rowH) + DpiScale(4);
    int width = DpiScale(360);
    HDC hdcScreen = GetDC(nullptr);
    if (hdcScreen) {
        PlatformFont* pfont = GetAppMenuFont();
        HFONT font = pfont ? pfont->GetHFont() : nullptr;
        HFONT old = font ? (HFONT)SelectObject(hdcScreen, font) : nullptr;
        SIZE sz{};
        TempWStr wStats = ToWStrTemp(st->stats);
        GetTextExtentPoint32W(hdcScreen, wStats.s, wStats.len, &sz);
        width = std::max(width, (int)sz.cx + st->padX * 2);
        if (st->hasCloseAll) {
            TempWStr wClose = ToWStrTemp(st->closeAllLabel);
            GetTextExtentPoint32W(hdcScreen, wClose.s, wClose.len, &sz);
            width = std::max(width, (int)sz.cx + st->padX * 2);
        }
        for (Str& title : st->titles) {
            TempWStr wTitle = ToWStrTemp(title);
            GetTextExtentPoint32W(hdcScreen, wTitle.s, wTitle.len, &sz);
            width = std::max(width, (int)sz.cx + st->padX * 2 + DpiScale(24));
        }
        if (old) {
            SelectObject(hdcScreen, old);
        }
        ReleaseDC(nullptr, hdcScreen);
    }
    width = std::min(width, DpiScale(560));

    POINT pt;
    GetCursorPos(&pt);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SUMATRA_WEBPANEL_TABS_MENU", L"",
                                WS_POPUP | WS_BORDER, pt.x, pt.y, width, height, win->hwndFrame, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        delete st;
        return;
    }
    st->hwnd = hwnd;
    gTabsPopup = st;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void OnTabsButton(MainWindow* win) {
    ShowTabsMenu(win);
}

void OnNotebookLmTabButton(MainWindow* win) {
    if (!win) {
        return;
    }
    win->uiState.aiChatVisible = false;
    win->uiState.webPanelVisible = true;
    EnsureWebPanelWebView(win);
    RestorePdfNotebookLmTab(win);
    ScheduleUiUpdate(win);
    WriteBridgeJson(win);
}

void OnWebPanelJsNotify(void* ctx, Str method, Str paramsJson) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !method) {
        return;
    }
    if (str::Eq(method, "docTitle")) {
        Str title = {};
        auto grab = [](Str* out, json::Value* v) {
            if (v->type == json::Type::String && json::PathMatch(v->path, StrL("i0"))) {
                *out = str::Dup(v->value);
                v->stop = true;
            }
        };
        json::Parse(paramsJson, MkFunc1<Str, json::Value*>(grab, &title));
        if (title && win->webPanelWebView) {
            TempStr url = win->webPanelWebView->GetSourceTemp();
            SyncWebPanelTabFromWebView(win, win->webPanelWebView, url, title);
        }
        str::Free(title);
    }
}

void CloseWebPanelFromLabel(MainWindow* win) {
    CloseWebPanel(win);
}

bool OnWebNavStarting(void* ctx, Str url, bool newWindow) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win)) {
        return true;
    }
    if (newWindow && url) {
        // Popup / target=_blank → new tab (keep existing tabs).
        CreateNewWebPanelTab(win, url, TitleFromUrlTemp(url));
        return false;
    }
    return true;
}

void OnWebNavCompleted(void* ctx, Str url, bool success) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !success) {
        return;
    }
    WebviewWnd* wv = win->webPanelWebView;
    TempStr title = wv ? wv->GetDocumentTitleTemp() : TempStr{};
    SyncWebPanelTabFromWebView(win, wv, url, title);
    if (win->webPanelWebView) {
        win->webPanelWebView->SetControllerVisible(true);
        if (win->webPanelWebView->emulateMobile) {
            win->webPanelWebView->ApplyMobileEmulation();
        }
    }
    RelayoutWebPanel(win);
    WriteBridgeJson(win);
}

int FindWebPanelTabByWebView(MainWindow* win, WebviewWnd* wv) {
    if (!win || !wv) {
        return -1;
    }
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        if (win->webPanelTabs[i].wv == wv) {
            return i;
        }
    }
    return -1;
}

void SyncWebPanelTabFromWebView(MainWindow* win, WebviewWnd* wv, Str url, Str title) {
    if (!IsMainWindowValid(win)) {
        return;
    }
    int idx = FindWebPanelTabByWebView(win, wv);
    if (idx < 0) {
        idx = win->webPanelActiveTab;
    }
    if (idx < 0 || idx >= len(win->webPanelTabs)) {
        return;
    }
    WebPanelTab& t = win->webPanelTabs[idx];
    bool changed = false;
    if (url && !str::StartsWithI(url, StrL("about:"))) {
        if (!str::Eq(t.url, url)) {
            str::ReplaceWithCopy(&t.url, url);
            changed = true;
        }
        if (idx == win->webPanelActiveTab) {
            str::ReplaceWithCopy(&win->webPanelCurrentUrl, url);
            str::ReplaceWithCopy(&gLastUrl, url);
        }
    }
    if (title && title.len > 0) {
        // Prefer the live document.title (Chrome tab label); skip empty / URL-looking fallbacks.
        bool titleIsUrl = str::StartsWithI(title, StrL("http://")) || str::StartsWithI(title, StrL("https://"));
        if (!titleIsUrl && !str::Eq(t.title, title)) {
            str::ReplaceWithCopy(&t.title, title);
            changed = true;
        }
    }
    if (changed) {
        SaveWebPanelTabs(win);
        if (idx == win->webPanelActiveTab) {
            RememberPdfActiveTab(win);
            WriteBridgeJson(win);
        }
    }
}

void OnWebSourceChanged(void* ctx, WebviewWnd* sender, Str url) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !sender) {
        return;
    }
    TempStr title = sender->GetDocumentTitleTemp();
    SyncWebPanelTabFromWebView(win, sender, url, title);
}

void OnWebDocumentTitleChanged(void* ctx, WebviewWnd* sender, Str title) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !sender) {
        return;
    }
    TempStr url = sender->GetSourceTemp();
    SyncWebPanelTabFromWebView(win, sender, url, title);
}

void EnsureWebPanelWebView(MainWindow* win) {
    if (!win || !HasWebView()) {
        return;
    }
    LoadBookmarks();
    LoadWebPanelTabs(win);
    if (win->webPanelWebViewReady && win->webPanelWebView) {
        ShowActiveWebPanelTab(win);
        return;
    }
    if (win->webPanelActiveTab >= 0 && win->webPanelActiveTab < len(win->webPanelTabs)) {
        ActivateWebPanelTabByIndex(win, win->webPanelActiveTab, false);
        return;
    }
    if (len(win->webPanelTabs) > 0) {
        ActivateWebPanelTabByIndex(win, 0, false);
        return;
    }
    TempStr url = gLastUrl;
    if (!url || str::EqI(url, StrL("about:blank")) || str::EqI(url, StrL("about:blank/"))) {
        url = Str(kDefaultBookmarks[0].url);
    }
    CreateNewWebPanelTab(win, url, TitleFromUrlTemp(url));
}

LRESULT CALLBACK WndProcWebPanelBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        return CallWindowProcW(gWebPanelBoxWndProc, hwnd, msg, wp, lp);
    }
    LRESULT res = 0;
    res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }
    if (VirtHostOnMessage(hwnd, win->webPanelRoot, msg, wp, lp, res, ThemeControlBackgroundColor())) {
        return res;
    }
    if (msg == WM_SIZE) {
        LayoutWebPanelBox(win);
        return 0;
    }
    return CallWindowProcW(gWebPanelBoxWndProc, hwnd, msg, wp, lp);
}

static const char* kIconRefresh =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><path fill="currentColor" d="M13.65 2.35A7.95 7.95 0 0 0 8 0a8 8 0 1 0 7.5 10h-1.6A6.4 6.4 0 1 1 8 1.6c1.5 0 2.9.5 4 1.4L9.5 5.5H16V0l-2.35 2.35z"/></svg>)";

VirtIconButton* HeaderIconButton(const char* svg, Str tooltip, const VirtMouseHandler& onClick) {
    int sz = DpiScale(kPinIconPx);
    auto* button = new VirtIconButton();
    button->pixmap = GetCachedPixmapForSvg(Str(svg), sz, sz);
    int pad = DpiScale(2);
    button->padding = Insets{0, pad, 0, pad};
    button->onClick = onClick;
    button->SetTooltip(tooltip);
    return button;
}

} // namespace

void WebPanelClearPdfTabIds(i64 bookId) {
    UpsertPdfTabBinding(bookId, {}, {}, {}, {}, false, false, true, false);
}

void WebPanelClearPdfTabUrls(i64 bookId) {
    UpsertPdfTabBinding(bookId, {}, {}, {}, {}, false, false, false, true);
}

void RelayoutWebPanel(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    LayoutWebPanelBox(win);
    if (win->webPanelWebView && win->webPanelWebViewReady) {
        win->webPanelWebView->UpdateWebviewSize();
    }
    RedrawWindow(win->hwndWebPanelBox, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void CreateWebPanel(MainWindow* win) {
    if (!HasWebView()) {
        return;
    }
    EnsureWebPanelDataLayout();
    UpdateWebPanelCpuSample();
    LoadBookmarks();

    int dx = gGlobalPrefs->aiChatSidebarDx > 0 ? gGlobalPrefs->aiChatSidebarDx : 360;
    win->webPanelDx = dx;
    DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    win->hwndWebPanelBox = CreateWindowExW(0, WC_STATICW, L"", style, 0, 0, dx, 0, win->hwndFrame, nullptr,
                                           GetModuleHandleW(nullptr), nullptr);

    PlatformFont* labelFont = GetAppSidebarLabelFont();
    auto header = NewLabelWithClose(win->hwndWebPanelBox, labelFont, MkFunc0(CloseWebPanelFromLabel, win));
    win->webPanelLabel = header.label;
    header.label->SetText(_TRA("AI"));
    win->webPanelBookmarksBtn = HeaderIconButton(gIconBookmarks, _TRA("书签（点击新建 Tab）"),
                                                 MkFunc0(OnBookmarksButton, win));
    win->webPanelTabsBtn = HeaderIconButton(gIconTabs, _TRA("Tab 列表"), MkFunc0(OnTabsButton, win));
    win->webPanelNotebookLmBtn =
        HeaderIconButton(gIconNotebookLm, _TRA("当前 PDF 的 NotebookLM Tab"), MkFunc0(OnNotebookLmTabButton, win));
    win->webPanelFocusPdfBtn =
        HeaderIconButton(gIconTargetFocus, _TRA("仅与当前 PDF 对话"), MkFunc0(OnFocusCurrentPdfNotebookLm, win));
    win->webPanelRefreshBtn = HeaderIconButton(kIconRefresh, _TRA("Refresh"), MkFunc0(OnWebPanelRefresh, win));

    // AI | bookmarks | tabs | notebooklm | focus-pdf | (spacer) | refresh | close
    if (len(header.box->children) > 0) {
        header.box->children[0].flex = 0;
        header.box->children.Pop();
    }
    header.box->AddChild(win->webPanelBookmarksBtn);
    header.box->AddChild(win->webPanelTabsBtn);
    header.box->AddChild(win->webPanelNotebookLmBtn);
    header.box->AddChild(win->webPanelFocusPdfBtn);
    header.box->AddChild(new Spacer(0, 0), 1);
    header.box->AddChild(win->webPanelRefreshBtn);
    header.box->AddChild(header.closeBtn);
    win->webPanelHeader = header.box;

    auto* sep = new VirtLine();
    sep->thickness = 1;
    // use theme edge (same as Library / Bookmarks separators), not pure black

    win->webPanelWebView = nullptr;
    win->webPanelWebViewReady = false;
    win->webPanelActiveTab = -1;
    win->webPanelWebViewSlot = new Spacer(0, 0);

    auto* vbox = new VBox();
    vbox->alignCross = CrossAxisAlign::Stretch;
    vbox->AddChild(win->webPanelHeader);
    vbox->AddChild(sep);
    vbox->AddChild(win->webPanelWebViewSlot, 1);
    win->webPanelLayout = vbox;

    if (!gWebPanelBoxWndProc) {
        gWebPanelBoxWndProc = (WNDPROC)GetWindowLongPtrW(win->hwndWebPanelBox, GWLP_WNDPROC);
    }
    SetWindowLongPtrW(win->hwndWebPanelBox, GWLP_WNDPROC, (LONG_PTR)WndProcWebPanelBox);
    UpdateWebPanelTheme(win);

    if (win->webPanelSplitter) {
        win->webPanelSplitter->SetIsVisible(false);
        win->webPanelSplitter->onMove = MkFunc1Void(OnWebPanelSplitterMove);
    }

    // default open; defer WebView2 so the main window can Show immediately
    win->uiState.webPanelVisible = true;
    win->uiState.aiChatVisible = false;
    uitask::Post(MkFunc0(DeferredEnsureWebPanelWebView, win), "EnsureWebPanelWebView");
}

void DestroyWebPanel(MainWindow* win) {
    if (!win) {
        return;
    }
    SaveWebPanelTabs(win);
    FreePinClickCtxs();
    for (WebPanelTab& tab : win->webPanelTabs) {
        delete tab.wv;
        tab.wv = nullptr;
        str::Free(tab.id);
        str::Free(tab.url);
        str::Free(tab.title);
        tab.id = {};
        tab.url = {};
        tab.title = {};
    }
    win->webPanelTabs.Reset();
    win->webPanelActiveTab = -1;
    win->webPanelWebView = nullptr;
    win->webPanelWebViewReady = false;
    delete win->webPanelLayout;
    win->webPanelLayout = nullptr;
    delete win->webPanelRoot;
    win->webPanelRoot = nullptr;
    win->webPanelHeader = nullptr;
    win->webPanelLabel = nullptr;
    win->webPanelBookmarksBtn = nullptr;
    win->webPanelTabsBtn = nullptr;
    win->webPanelNotebookLmBtn = nullptr;
    win->webPanelFocusPdfBtn = nullptr;
    win->webPanelRefreshBtn = nullptr;
    win->webPanelWebViewSlot = nullptr;
    if (win->webPanelSplitter) {
        win->webPanelSplitter->onMove = {};
        win->webPanelSplitter->SetIsVisible(false);
    }
    str::Free(win->webPanelCurrentUrl);
    win->webPanelCurrentUrl = {};
    if (win->hwndWebPanelBox) {
        DestroyWindow(win->hwndWebPanelBox);
        win->hwndWebPanelBox = nullptr;
    }
}

void CloseWebPanel(MainWindow* win) {
    if (!win) {
        return;
    }
    win->uiState.webPanelVisible = false;
    ScheduleUiUpdate(win);
}

// Forward decls used by WebPanelOnDocumentChanged (defined below).
void WebPanelEnsureNotebookLmVisible(MainWindow* win);
void WebPanelSpawnScript(Str scriptName, Str extraArgs);

void OnWebPanelToggle(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    if (win->uiState.webPanelVisible) {
        CloseWebPanel(win);
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (tab) {
        AIChatSetTabPanelOpen(tab, AIChatBackend::None);
    }
    AIChatSyncPanelsToCurrentTab(win);
    win->uiState.aiChatVisible = false;
    win->uiState.webPanelVisible = true;
    EnsureWebPanelWebView(win);
    ScheduleUiUpdate(win);
}

void WebPanelOnDocumentChanged(MainWindow* win) {
    if (!win || !win->uiState.webPanelVisible) {
        return;
    }
    WriteBridgeJson(win);
    // Switch to the Tab remembered for this PDF (browser-like). No record → leave as-is.
    RestorePdfActiveTab(win);
}

TempStr ScriptsWebviewDirTemp() {
    TempStr od = path::JoinTemp(GetOneDriveAppDataDirTemp(), StrL("scripts\\webview"));
    if (file::Exists(path::JoinTemp(od, StrL("package.json")))) {
        return od;
    }
    TempStr repo = StrL("C:\\workspace\\sumatrapdf\\scripts\\webview");
    if (file::Exists(path::JoinTemp(repo, StrL("package.json")))) {
        return repo;
    }
    return od;
}

TempStr FindNodeExeTemp() {
    TempStr envNode = GetEnvVariableTemp(StrL("SUMATRA_NODE"));
    if (envNode && file::Exists(envNode)) {
        return envNode;
    }
    TempStr candidates[] = {
        path::JoinTemp(GetEnvVariableTemp(StrL("ProgramFiles")), StrL("nodejs\\node.exe")),
        path::JoinTemp(GetEnvVariableTemp(StrL("LOCALAPPDATA")), StrL("Programs\\node\\node.exe")),
    };
    for (TempStr c : candidates) {
        if (c && file::Exists(c)) {
            return c;
        }
    }
#ifdef _MSC_VER
    WCHAR pathW[MAX_PATH];
    if (SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, pathW, nullptr) > 0) {
        return ToUtf8Temp(pathW);
    }
#endif
    return {};
}

void NotebookLmPollLoopThread() {
    for (int i = 0; i < 90; i++) {
        Sleep(2000);
        uitask::Post(MkFunc0Void(WebPanelPollBridgeResults), "NotebookLmPoll");
    }
}

void ScheduleNotebookLmResultPolls() {
    // Playwright uploads can take minutes; keep polling done/ without a delay API.
    RunAsync(MkFunc0Void(NotebookLmPollLoopThread), "NotebookLmPollLoop");
}

void WebPanelEnsureNotebookLmVisible(MainWindow* win) {
    if (!win) {
        return;
    }
    win->uiState.aiChatVisible = false;
    win->uiState.webPanelVisible = true;
    EnsureWebPanelWebView(win);
    // Prefer this PDF's remembered NotebookLM tab (stable id → url fallback).
    RestorePdfNotebookLmTab(win);
    ScheduleUiUpdate(win);
    WriteBridgeJson(win);
}

void WebPanelSpawnScript(Str scriptName, Str extraArgs) {
    TempStr dir = ScriptsWebviewDirTemp();
    TempStr script = path::JoinTemp(dir, scriptName);
    if (!file::Exists(script)) {
        logf("WebPanelSpawnScript: missing '%s'\n", script);
        return;
    }
    TempStr node = FindNodeExeTemp();
    if (!node || !file::Exists(node)) {
        logf("WebPanelSpawnScript: node.exe not found (set SUMATRA_NODE). script='%s'\n", script);
        return;
    }
    TempStr cmd =
        extraArgs ? fmt("\"%s\" \"%s\" %s", node, script, extraArgs) : fmt("\"%s\" \"%s\"", node, script);
    // Hide node.exe console (CREATE_NO_WINDOW + SW_HIDE + NUL stdio).
    HANDLE h = LaunchProcessHidden(cmd, dir);
    if (!h) {
        logf("WebPanelSpawnScript: CreateProcess failed node='%s' cmd='%s'\n", node, cmd);
        return;
    }
    CloseHandle(h);
    logf("WebPanelSpawnScript: ok (hidden) node='%s' script='%s'\n", node, scriptName);
}

void WebPanelAddPdfToNotebookLm(MainWindow* win, i64 bookId, Str pdfPath, Str title) {
    if (!pdfPath) {
        return;
    }
    if (win) {
        WebPanelEnsureNotebookLmVisible(win);
    }
    TempStr jobs = WebPanelJobsPendingTemp();
    dir::CreateAll(jobs);
    dir::CreateAll(WebPanelJobsDoneTemp());
    dir::CreateAll(WebPanelJobsFailedTemp());

    // Include prior notebooklm mapping so re-add can jump back to the same notebook
    // and always rewrite books.notebooklm with the latest placement.
    TempStr priorJson = {};
    if (bookId > 0) {
        Str blob = LibraryStoreGetBookNotebookLm(LibraryGetStore(), bookId);
        if (blob) {
            priorJson = str::DupTemp(blob);
            str::Free(blob);
        }
    }

    TempStr jobPath = path::JoinTemp(jobs, fmt("add-%lld.json", (i64)UnixTimeMsNow()));
    TempStr body = fmt(
        "{\n  \"action\": \"notebooklm.add\",\n  \"bookId\": %lld,\n  \"pdfPath\": %s,\n  \"title\": %s,\n"
        "  \"notebooklm\": %s\n}\n",
        bookId, EscapeJsonTemp(pdfPath), EscapeJsonTemp(title),
        priorJson ? EscapeJsonTemp(priorJson) : StrL("\"\""));
    file::WriteFile(jobPath, body);
    TempStr args = fmt("--job \"%s\"", jobPath);
    WebPanelSpawnScript(StrL("notebooklm-add.mjs"), args);
    ScheduleNotebookLmResultPolls();
    logf("WebPanelAddPdfToNotebookLm: queued '%s'\n", jobPath);
}

void WebPanelSelectNotebookLmSource(MainWindow* win, i64 bookId, Str pdfPath, Str title) {
    if (!pdfPath) {
        return;
    }
    if (win) {
        WebPanelEnsureNotebookLmVisible(win);
    }
    TempStr nbUrl = {};
    TempStr json = {};
    if (bookId > 0) {
        Str blob = LibraryStoreGetBookNotebookLm(LibraryGetStore(), bookId);
        if (blob) {
            json = str::DupTemp(blob);
            str::Free(blob);
            Str key = StrL("\"notebookUrl\"");
            int k = str::IndexOfI(json, key);
            if (k >= 0) {
                Str rest = Str(json.s + k + key.len, json.len - k - key.len);
                int q1 = str::IndexOfChar(rest, '"');
                if (q1 >= 0) {
                    Str after = Str(rest.s + q1 + 1, rest.len - q1 - 1);
                    int q2 = str::IndexOfChar(after, '"');
                    if (q2 > 0) {
                        nbUrl = str::DupTemp(Str(after.s, q2));
                    }
                }
            }
        }
    }
    TempStr sourceTitle = title && title.len > 0 ? title : path::GetBaseNameTemp(pdfPath);
    TempStr jobs = WebPanelJobsPendingTemp();
    dir::CreateAll(jobs);
    TempStr jobPath = path::JoinTemp(jobs, fmt("select-%lld.json", (i64)UnixTimeMsNow()));
    TempStr body = fmt(
        "{\n  \"action\": \"notebooklm.select\",\n  \"bookId\": %lld,\n  \"pdfPath\": %s,\n"
        "  \"fileName\": %s,\n  \"sourceTitle\": %s,\n  \"notebookUrl\": %s,\n  \"notebooklm\": %s\n}\n",
        bookId, EscapeJsonTemp(pdfPath), EscapeJsonTemp(sourceTitle), EscapeJsonTemp(sourceTitle),
        EscapeJsonTemp(nbUrl), EscapeJsonTemp(json));
    file::WriteFile(jobPath, body);
    TempStr args = fmt("--job \"%s\"", jobPath);
    WebPanelSpawnScript(StrL("notebooklm-select.mjs"), args);
    logf("WebPanelSelectNotebookLmSource: queued '%s'\n", jobPath);
}

TempStr WebPanelDbgControlTemp(Str action, Str a, Str b, int n1, int n2, int* exitCodeOut) {
    (void)n2;
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
        TempStr url = win->webPanelCurrentUrl ? win->webPanelCurrentUrl : gLastUrl;
        int port = win->webPanelCdpPort > 0 ? win->webPanelCdpPort : gCdpPort;
        return finish(fmt("OK visible=%d tabs=%d cdp=%d url=%s bridge=%s", win->uiState.webPanelVisible ? 1 : 0,
                          len(win->webPanelTabs), port, url ? url : StrL(""), BridgePathTemp()),
                      0);
    }

    if (str::EqI(action, "show")) {
        WebPanelEnsureNotebookLmVisible(win);
        return finish(fmt("OK visible=1 cdp=%d", win->webPanelCdpPort > 0 ? win->webPanelCdpPort : gCdpPort), 0);
    }

    if (str::EqI(action, "hide")) {
        CloseWebPanel(win);
        return finish(StrL("OK visible=0"), 0);
    }

    if (str::EqI(action, "poll")) {
        WebPanelPollBridgeResults();
        return finish(StrL("OK polled"), 0);
    }

    if (str::EqI(action, "add")) {
        // a=pdfPath, n1=bookId (optional), b=title (optional)
        if (!a) {
            return finish(StrL("ERROR add expects pdfPath [bookId] [title]"), 1);
        }
        i64 bookId = n1 > 0 ? n1 : 0;
        if (bookId <= 0) {
            LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), a);
            if (book) {
                bookId = book->id;
                DeleteLibraryBook(book);
            }
        }
        TempStr title = b && b.len > 0 ? b : path::GetBaseNameTemp(a);
        WebPanelAddPdfToNotebookLm(win, bookId, a, title);
        return finish(fmt("OK queued bookId=%lld path=%s", bookId, a), 0);
    }

    if (str::EqI(action, "notebooklm")) {
        // a=pdfPath optional, n1=bookId optional; else current tab path
        TempStr path = a;
        if ((!path || path.len == 0) && n1 <= 0) {
            WindowTab* tab = win->CurrentTab();
            if (tab && tab->filePath) {
                path = tab->filePath;
            }
        }
        if (!path || path.len == 0) {
            return finish(StrL("ERROR notebooklm expects path or current document"), 1);
        }
        LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), path);
        if (!book) {
            return finish(StrL("ERROR no-book"), 1);
        }
        TempStr json = book->notebooklm ? book->notebooklm : StrL("");
        i64 id = book->id;
        DeleteLibraryBook(book);
        return finish(fmt("OK bookId=%lld notebooklm=%s", id, json), 0);
    }

    return finish(fmt("ERROR unknown-action %s", action ? action : StrL("")), 1);
}

void WebPanelPollBridgeResults() {
    UpdateWebPanelCpuSample();
    TempStr doneDir = WebPanelJobsDoneTemp();
    if (!dir::Exists(doneDir)) {
        return;
    }
    DirIter di{doneDir};
    for (DirIterEntry* e : di) {
        if (!e || e->isDir || !str::EndsWithI(e->name, StrL(".json"))) {
            continue;
        }
        TempStr path = e->filePath ? e->filePath : path::JoinTemp(doneDir, e->name);
        Str data = file::ReadFile(path);
        if (!data) {
            continue;
        }
        auto grab = [](Str json, Str key) -> TempStr {
            int k = str::IndexOfI(json, key);
            if (k < 0) {
                return {};
            }
            Str rest = Str(json.s + k + key.len, json.len - k - key.len);
            int q1 = str::IndexOfChar(rest, '"');
            if (q1 < 0) {
                return {};
            }
            Str after = Str(rest.s + q1 + 1, rest.len - q1 - 1);
            int q2 = str::IndexOfChar(after, '"');
            if (q2 <= 0) {
                return {};
            }
            return str::DupTemp(Str(after.s, q2));
        };
        i64 bookId = 0;
        {
            int k = str::IndexOfI(data, StrL("\"bookId\""));
            if (k >= 0) {
                Str rest = Str(data.s + k, data.len - k);
                int colon = str::IndexOfChar(rest, ':');
                if (colon >= 0) {
                    bookId = atoi(rest.s + colon + 1);
                }
            }
        }
        TempStr notebook = grab(data, StrL("\"notebook\""));
        TempStr notebookUrl = grab(data, StrL("\"notebookUrl\""));
        TempStr sourceTitle = grab(data, StrL("\"sourceTitle\""));
        bool okFlag = str::ContainsI(data, StrL("\"ok\": true")) || str::ContainsI(data, StrL("\"ok\":true")) ||
                      (notebook && notebookUrl);
        if (okFlag && bookId > 0 && notebook) {
            TempStr json = fmt(
                "{\"notebook\":%s,\"notebookUrl\":%s,\"sourceTitle\":%s,\"updatedMs\":%lld}",
                EscapeJsonTemp(notebook), EscapeJsonTemp(notebookUrl), EscapeJsonTemp(sourceTitle),
                (i64)UnixTimeMsNow());
            LibraryStoreSetBookNotebookLm(LibraryGetStore(), bookId, json);
            logf("WebPanelPollBridgeResults: book %lld -> %s\n", bookId, notebook);
        }
        str::Free(data);
        file::Delete(path);
    }
}

void UpdateWebPanelDpi(MainWindow* win, int dpi) {
    if (!win || !win->hwndWebPanelBox || dpi <= 0) {
        return;
    }
    if (win->webPanelLabel) {
        win->webPanelLabel->font = GetAppSidebarLabelFontForDpi(dpi);
    }
    RelayoutWebPanel(win);
}

void UpdateWebPanelTheme(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    DarkModeApplyToChildControls(win->hwndWebPanelBox);
    RedrawWindow(win->hwndWebPanelBox, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool IsWebPanelVisible(MainWindow* win) {
    return win && win->uiState.webPanelVisible;
}
