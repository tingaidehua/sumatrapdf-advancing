// Library sidebar: drive show/hide, open, tab switch, shelf/place, and
// high-frequency PDF switches through -dbg-control TestLibrary. No mouse.
//
// Run: bun tests/library-sidebar.ts [--no-build]

import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control.ts";
import { cmdId, EXE, makeMinimalPdf, runStandalone, tmpPath } from "./util.ts";

function makePdfWithOutline(title: string): Buffer {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const objs = [
    "<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> >>",
    `<< /Title (${title}) >>`,
    "<< /Type /Outlines /First 6 0 R /Last 6 0 R /Count 1 >>",
    "<< /Title (Introduction) /Parent 5 0 R >>",
  ];
  const parts: Buffer[] = [enc("%PDF-1.7\n")];
  const offsets: number[] = [];
  let pos = parts[0]!.length;
  objs.forEach((body, i) => {
    offsets.push(pos);
    const obj = enc(`${i + 1} 0 obj\n${body}\nendobj\n`);
    parts.push(obj);
    pos += obj.length;
  });
  let xref = `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) {
    xref += `${String(off).padStart(10, "0")} 00000 n \n`;
  }
  parts.push(enc(`${xref}trailer\n<< /Size ${objs.length + 1} /Root 1 0 R /Info 4 0 R >>\nstartxref\n${pos}\n%%EOF\n`));
  return Buffer.concat(parts);
}

function makePdfWithPages(title: string, pageCount: number): Buffer {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const n = Math.max(1, pageCount);
  const kids = Array.from({ length: n }, (_, i) => `${i + 3} 0 R`).join(" ");
  const objs = [
    "<< /Type /Catalog /Pages 2 0 R >>",
    `<< /Type /Pages /Kids [${kids}] /Count ${n} >>`,
    ...Array.from({ length: n }, () => "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> >>"),
    `<< /Title (${title}) >>`,
  ];
  const parts: Buffer[] = [enc("%PDF-1.7\n")];
  const offsets: number[] = [];
  let pos = parts[0]!.length;
  objs.forEach((body, i) => {
    offsets.push(pos);
    const obj = enc(`${i + 1} 0 obj\n${body}\nendobj\n`);
    parts.push(obj);
    pos += obj.length;
  });
  let xref = `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) {
    xref += `${String(off).padStart(10, "0")} 00000 n \n`;
  }
  const infoObj = objs.length;
  parts.push(
    enc(`${xref}trailer\n<< /Size ${objs.length + 1} /Root 1 0 R /Info ${infoObj} 0 R >>\nstartxref\n${pos}\n%%EOF\n`),
  );
  return Buffer.concat(parts);
}

function field(raw: string, name: string): string {
  const m = new RegExp(`${name}=(\\S+)`).exec(raw);
  if (!m) {
    throw new Error(`library-sidebar: missing ${name}= in '${raw}'`);
  }
  return m[1];
}

function parseToolbar(raw: string): Map<number, { hidden: boolean; x: number }> {
  const out = new Map<number, { hidden: boolean; x: number }>();
  for (const m of raw.matchAll(/idx=\d+ cmd=(\d+) hidden=(\d+) rect=(-?\d+),/g)) {
    out.set(Number(m[1]), { hidden: m[2] === "1", x: Number(m[3]) });
  }
  return out;
}

async function withTimeout<T>(p: Promise<T>, ms: number, msg: string): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      p,
      new Promise<T>((_, reject) => {
        timer = setTimeout(() => reject(new Error(msg)), ms);
      }),
    ]);
  } finally {
    if (timer) {
      clearTimeout(timer);
    }
  }
}

export async function testit(): Promise<void> {
  const dir = tmpPath(`library-sidebar-${process.pid}`);
  mkdirSync(dir, { recursive: true });
  const noTocA = join(dir, "alpha-no-toc.pdf");
  const noTocB = join(dir, "beta-no-toc.pdf");
  const withToc = join(dir, "gamma-with-toc.pdf");
  const manyPages = join(dir, "delta-many-pages.pdf");
  writeFileSync(noTocA, makeMinimalPdf("alpha"));
  writeFileSync(noTocB, makeMinimalPdf("beta"));
  writeFileSync(withToc, makePdfWithOutline("gamma"));
  writeFileSync(manyPages, makePdfWithPages("delta", 120));

  await withControlledSumatra(EXE, async (client) => {
    const lib = (action: string, ...args: (string | number)[]) =>
      withTimeout(client.library(action, ...args), 15000, `TestLibrary ${action} timed out (UI thread hung?)`);
    const toolbar = async () => {
      const res = await client.request(ControlCommand.TestToolbarButtons, []);
      const code = typeof res[0] === "number" ? res[0] : -1;
      const raw = String(res[1] ?? "");
      if (code !== 0) {
        throw new Error(`library-sidebar: TestToolbarButtons failed: ${raw}`);
      }
      return parseToolbar(raw);
    };

    let r = await lib("status");
    if (r.code === 2) {
      throw new Error(`library-sidebar: not ready: ${r.raw}`);
    }

    r = await lib("open", noTocA);
    if (r.code !== 0) {
      throw new Error(`library-sidebar: open A failed: ${r.raw}`);
    }
    r = await lib("open", noTocB);
    if (r.code !== 0) {
      throw new Error(`library-sidebar: open B failed: ${r.raw}`);
    }
    r = await lib("open", withToc);
    if (r.code !== 0) {
      throw new Error(`library-sidebar: open C failed: ${r.raw}`);
    }

    r = await lib("show");
    if (r.code !== 0 || field(r.raw, "libraryVis") !== "1") {
      throw new Error(`library-sidebar: show failed: ${r.raw}`);
    }
    // Opening the library must not force the bookmarks pane open.
    const tocAfterShow = field(r.raw, "tocVis");
    const showTocAfterShow = field(r.raw, "showToc");
    if (tocAfterShow !== showTocAfterShow) {
      throw new Error(`library-sidebar: library hold still forcing bookmarks: ${r.raw}`);
    }
    const libraryDx = field(r.raw, "libraryDx");

    r = await lib("tabs");
    if (r.code !== 0 || !r.raw.includes("count=")) {
      throw new Error(`library-sidebar: tabs failed: ${r.raw}`);
    }
    const tabs = [...r.raw.matchAll(/tab idx=(\d+) current=(\d+) path=(.+\.pdf)/gi)].map((m) => ({
      idx: Number(m[1]),
      current: m[2] === "1",
      path: m[3]!,
    }));
    if (tabs.length < 3) {
      throw new Error(`library-sidebar: expected document tabs, got: ${r.raw}`);
    }
    const idxA = tabs.find((t) => t.path.includes("alpha-no-toc"))?.idx;
    const idxB = tabs.find((t) => t.path.includes("beta-no-toc"))?.idx;
    const idxC = tabs.find((t) => t.path.includes("gamma-with-toc"))?.idx;
    if (idxA === undefined || idxB === undefined || idxC === undefined) {
      throw new Error(`library-sidebar: missing A/B/C tabs: ${r.raw}`);
    }

    r = await lib("switch-tab", idxC);
    if (r.code !== 0) {
      throw new Error(`library-sidebar: switch C failed: ${r.raw}`);
    }
    r = await lib("toc-show");
    if (r.code !== 0 || field(r.raw, "tocVis") !== "1" || field(r.raw, "showToc") !== "1") {
      throw new Error(`library-sidebar: toc-show C failed: ${r.raw}`);
    }

    r = await lib("switch-tab", idxB);
    if (r.code !== 0) {
      throw new Error(`library-sidebar: switch B failed: ${r.raw}`);
    }
    r = await lib("toc-hide");
    if (r.code !== 0 || field(r.raw, "tocVis") !== "0" || field(r.raw, "showToc") !== "0") {
      throw new Error(`library-sidebar: toc-hide B failed: ${r.raw}`);
    }

    r = await lib("switch-tab", idxC);
    if (r.code !== 0 || field(r.raw, "tocVis") !== "1" || field(r.raw, "showToc") !== "1") {
      throw new Error(`library-sidebar: C should keep bookmarks open: ${r.raw}`);
    }
    r = await lib("switch-tab", idxB);
    if (r.code !== 0 || field(r.raw, "tocVis") !== "0" || field(r.raw, "showToc") !== "0") {
      throw new Error(`library-sidebar: B should keep bookmarks closed: ${r.raw}`);
    }
    if (field(r.raw, "libraryDx") !== libraryDx) {
      throw new Error(`library-sidebar: libraryDx moved: ${libraryDx}->${field(r.raw, "libraryDx")}`);
    }

    // No-ToC PDFs still show the bookmarks toolbar button, and later buttons
    // stay put when the page-count digits change.
    let buttons = await toolbar();
    const bookmarks = buttons.get(cmdId("CmdToggleBookmarks"));
    if (!bookmarks || bookmarks.hidden) {
      throw new Error(`library-sidebar: bookmarks button hidden on no-ToC PDF`);
    }
    const nextPageX = buttons.get(cmdId("CmdGoToNextPage"))?.x;
    if (nextPageX === undefined) {
      throw new Error(`library-sidebar: missing next-page button`);
    }

    r = await lib("open", manyPages);
    if (r.code !== 0) {
      throw new Error(`library-sidebar: open many-pages failed: ${r.raw}`);
    }
    buttons = await toolbar();
    const bookmarksMany = buttons.get(cmdId("CmdToggleBookmarks"));
    if (!bookmarksMany || bookmarksMany.hidden) {
      throw new Error(`library-sidebar: bookmarks button hidden on many-page PDF`);
    }
    if (bookmarksMany.x !== bookmarks.x) {
      throw new Error(`library-sidebar: bookmarks button moved ${bookmarks.x}->${bookmarksMany.x}`);
    }
    const nextPageXMany = buttons.get(cmdId("CmdGoToNextPage"))?.x;
    if (nextPageXMany !== nextPageX) {
      throw new Error(`library-sidebar: next-page button moved ${nextPageX}->${nextPageXMany}`);
    }

    r = await lib("stress-switch", 40);
    if (r.code !== 0) {
      throw new Error(`library-sidebar: stress-switch failed: ${r.raw}`);
    }
    const maxMs = Number(field(r.raw, "maxMs"));
    if (!Number.isFinite(maxMs) || maxMs > 5000) {
      throw new Error(`library-sidebar: switch too slow: ${r.raw}`);
    }

    r = await lib("list");
    if (r.code !== 0 || !r.raw.includes("book id=")) {
      throw new Error(`library-sidebar: list failed: ${r.raw}`);
    }
    const bookId = Number(/book id=(\d+)/.exec(r.raw)?.[1]);
    r = await lib("new-shelf", "StressShelf");
    if (r.code !== 0) {
      throw new Error(`library-sidebar: new-shelf failed: ${r.raw}`);
    }
    const shelfId = Number(/shelf=(\d+)/.exec(r.raw)?.[1]);
    if (!bookId || !shelfId) {
      throw new Error(`library-sidebar: missing ids book=${bookId} shelf=${shelfId} raw=${r.raw}`);
    }
    r = await lib("place", bookId, shelfId);
    if (r.code !== 0) {
      throw new Error(`library-sidebar: place failed: ${r.raw}`);
    }

    r = await lib("hide");
    if (r.code !== 0 || field(r.raw, "libraryVis") !== "0") {
      throw new Error(`library-sidebar: hide failed: ${r.raw}`);
    }
  });
}

if (import.meta.main) {
  await runStandalone(testit);
}
