// Builds packages/conformance (the conformance set shipped with the framework) from its widget files:
//   1. catalogue.json / catalogue.md from scripts/conformance/catalogue.ts;
//   2. the harness region of every widget (harness CSS + JS, stage geometry);
//   3. reference images: every stage rendered natively by headless Chromium
//      (and WebKit when Playwright has it installed) at DPR 2, in reference mode
//      (a stage with <template data-ref> gets the template's markup) →
//      packages/conformance/refs/<set>/<widget>/<stage>.<engine>[.dark].png;
//   4. the visual set's embedded reference images (<img data-ref-img>);
//   5. the native run of every widget (no template swap): what a plain
//      browser engine says to each check, recorded in the manifest as a sanity
//      reference for the tests themselves;
//   6. manifest.json: test → catalogue IDs, kind, stages and references,
//      actions, host checks, themes and widths, coverage per set.
//
//   corepack pnpm tsx scripts/conformance/build.ts [--only 04-layout] [--no-refs] [--engines chromium,webkit]

import fs from 'node:fs';
import zlib from 'node:zlib';
import path from 'node:path';
import { chromium, webkit, type Browser, type BrowserContext, type Page } from '@playwright/test';
import { PNG } from 'pngjs';
import { CATALOGUE } from './catalogue';
import { emitCatalogue } from './emit-catalogue';
import { AUTO_GRID, CONF, autoStagePos, listWidgets, parseWidget, perform, readJson, refPath, type Published, type Widget } from './lib';
import { diffStats } from '../../packages/conformance/tools/diff.mjs';

const args = process.argv.slice(2);
const opt = (name: string) => (args.includes('--' + name) ? args[args.indexOf('--' + name) + 1] : undefined);
const ONLY = opt('only');
const NO_REFS = args.includes('--no-refs');
const ENGINES = (opt('engines') ?? 'chromium,webkit').split(',');
const WIDTH = 390;
const DPR = 2;

const harnessDir = path.join(CONF, 'harness');
const HARNESS_JS = fs.readFileSync(path.join(harnessDir, 'harness.js'), 'utf8').replace(/^\/\/.*\n/gm, '').trim();
const HARNESS_CSS = fs.readFileSync(path.join(harnessDir, 'harness.css'), 'utf8').trim();
const BEGIN = '<!-- conformance:harness -->';
const END = '<!-- /conformance:harness -->';
// The client-SDK group (visual/sdk) uses the mini harness, so a widget fits a v1 mini-widget.
const MINI_JS = fs.readFileSync(path.join(harnessDir, 'mini.js'), 'utf8').replace(/^\/\/.*\n/gm, '').trim();
const MINI_CSS = fs.readFileSync(path.join(harnessDir, 'mini.css'), 'utf8').trim();
const MINI_BEGIN = '<!-- conformance:mini -->';
const MINI_END = '<!-- /conformance:mini -->';
const SDK_MOCK = fs.readFileSync(path.join(path.dirname(new URL(import.meta.url).pathname), 'sdk-mock.js'), 'utf8');
/** v1 mini-widgets (docs/tgw-format.md): at most 4 KB gzipped and 32 KB of HTML. */
const MINI_LIMIT = { gzip: 4096, raw: 32768 };
const isSdk = (w: Widget) => w.id.startsWith('visual/sdk/');

// ───────────── 2. harness region ─────────────

function stageCss(w: Widget): string {
  const rules: string[] = [];
  w.stages.forEach((s, i) => {
    if (w.set === 'auto') {
      const [x, y] = autoStagePos(i);
      rules.push(`[data-stage="${s.name}"]{left:${x}px;top:${y}px;width:${s.w}px;height:${s.h}px}`);
    } else rules.push(`[data-stage="${s.name}"]{width:${s.w}px;height:${s.h}px}`);
  });
  if (w.set === 'auto' && w.stages.length) {
    const rows = Math.ceil(w.stages.length / AUTO_GRID.perRow);
    rules.push(`.conf-stages{height:${AUTO_GRID.top + rows * AUTO_GRID.dy}px}`);
  }
  return rules.join('');
}

function withHarness(w: Widget): string {
  if (w.html.includes(MINI_BEGIN)) {
    const i = w.html.indexOf(MINI_BEGIN), j = w.html.indexOf(MINI_END);
    if (j < 0) throw new Error(`${w.id}: no ${MINI_END}`);
    return w.html.slice(0, i) + `${MINI_BEGIN}<style>${MINI_CSS.replace(/\n/g, '')}</style><script>${MINI_JS}</script>${MINI_END}` + w.html.slice(j + MINI_END.length);
  }
  const region = `${BEGIN}<style>${HARNESS_CSS.replace(/\n/g, '')}${stageCss(w)}</style><script>${HARNESS_JS}</script>${END}`;
  const i = w.html.indexOf(BEGIN), j = w.html.indexOf(END);
  if (i < 0 || j < 0) throw new Error(`${w.id}: no ${BEGIN} … ${END} region`);
  return w.html.slice(0, i) + region + w.html.slice(j + END.length);
}

// ───────────── native runs ─────────────

async function newContext(browser: Browser, theme: 'light' | 'dark'): Promise<BrowserContext> {
  const ctx = await browser.newContext({ viewport: { width: WIDTH, height: 2000 }, deviceScaleFactor: DPR, colorScheme: theme });
  // A native page has the network: nothing may leave it (the widgets never need it; links must not navigate).
  await ctx.route('**/*', (r) => (r.request().url().startsWith('data:') ? r.continue() : r.abort()));
  return ctx;
}

async function published(page: Page): Promise<Published | null> {
  return page.evaluate(() => { const v = document.documentElement.getAttribute('data-conformance'); return v ? JSON.parse(v) : null; });
}

async function waitStatus(page: Page, want: string[], timeout: number): Promise<Published | null> {
  const t0 = Date.now();
  let last: Published | null = null;
  while (Date.now() - t0 < timeout) {
    last = await published(page).catch(() => null);
    if (last && want.includes(last.status)) return last;
    await page.waitForTimeout(100);
  }
  return last;
}

async function openNative(ctx: BrowserContext, html: string, mode: 'ref' | 'native', sdkMock = false): Promise<Page> {
  const page = await ctx.newPage();
  // The mode goes into the HTML, before the harness. Links never navigate in a native reference run.
  const boot = `<script>window.__conformanceMode=${JSON.stringify(mode)};document.addEventListener('click',function(e){var a=e.target&&e.target.closest&&e.target.closest('a');if(a)e.preventDefault()},true)</script>` + (sdkMock ? `<script>${SDK_MOCK}</script>` : '');
  // Served from a real (fake) origin, not about:blank, so storage works as in a WebView.
  const url = 'https://conformance.test/widget.html';
  await page.route(url, (r) => r.fulfill({ contentType: 'text/html; charset=utf-8', body: html.replace(/<head>/i, '<head>' + boot) }));
  await page.goto(url, { waitUntil: 'load' });
  return page;
}

interface NativeResult { status: string; checks: [string, string, string, string][]; skipped: string[] }

async function runNative(ctx: BrowserContext, w: Widget, html: string): Promise<NativeResult> {
  const page = await openNative(ctx, html, 'native', isSdk(w));
  let pub = await waitStatus(page, ['done', 'awaiting'], 15_000);
  let skipped: string[] = [];
  let skippedIds: string[] = [];
  if (w.meta.actions?.length) {
    ({ skipped, ids: skippedIds } = await perform(page, w.meta.actions, {
      origin: async () => ({ x: 0, y: 0 }),
      setTheme: async (t) => { await page.emulateMedia({ colorScheme: t }); },
    }, () => published(page)));
    pub = await waitStatus(page, ['done'], 8_000);
  }
  await page.close();
  const checks = (pub?.checks ?? []).map((c) => (skippedIds.includes(c[0]) ? [c[0], c[1], 'skip', 'input not available natively'] : c) as [string, string, string, string]);
  return { status: pub?.status ?? 'none', checks, skipped };
}

async function renderRefs(browsers: Record<string, Browser>, w: Widget, html: string): Promise<Record<string, Record<string, string>>> {
  const out: Record<string, Record<string, string>> = {};
  if (!w.stages.length) return out;
  for (const [engine, browser] of Object.entries(browsers)) {
    for (const theme of ['light', 'dark'] as const) {
      const stages = w.stages.filter((s) => s.themes.includes(theme));
      if (!stages.length) continue;
      const ctx = await newContext(browser, theme);
      const page = await openNative(ctx, html, 'ref');
      await waitStatus(page, ['done', 'awaiting'], 15_000);
      await page.waitForTimeout(150);
      for (const s of stages) {
        const file = refPath(w, s.name, engine, theme);
        fs.mkdirSync(path.dirname(file), { recursive: true });
        const loc = page.locator(`[data-stage="${s.name}"]`);
        const buf = await loc.screenshot({ animations: 'allow', caret: 'hide' });
        writeIfChanged(file, buf);
        ((out[s.name] ??= {})[`${engine}${theme === 'dark' ? '.dark' : ''}`] = path.relative(CONF, file));
        if (w.set === 'visual' && engine === 'chromium') {
          const jpg = await loc.screenshot({ type: 'jpeg', quality: 82, animations: 'allow', caret: 'hide' });
          embedded.set(`${w.id}#${s.name}${theme === 'dark' ? '#dark' : ''}`, 'data:image/jpeg;base64,' + jpg.toString('base64'));
        }
      }
      await ctx.close();
    }
  }
  return out;
}

/** Client-SDK group: the whole widget as the reference picture posted next to it (mock SDK, DPR 2, at most 1200 CSS px tall). */
async function renderSdkRefs(browser: Browser, w: Widget, html: string): Promise<Record<string, string>> {
  const out: Record<string, string> = {};
  for (const theme of (w.meta.themes ?? ['light']) as ('light' | 'dark')[]) {
    const ctx = await newContext(browser, theme);
    const page = await openNative(ctx, html, 'ref', true);
    await waitStatus(page, ['done', 'awaiting'], 15_000);
    await page.waitForTimeout(300);
    const h = Math.min(1200, await page.evaluate(() => Math.ceil(document.documentElement.getBoundingClientRect().height)));
    const file = path.join(CONF, 'refs', w.set, `${w.stem}${theme === 'dark' ? '.dark' : ''}.png`);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    writeIfChanged(file, await page.screenshot({ clip: { x: 0, y: 0, width: WIDTH, height: Math.max(h, 40) }, animations: 'disabled', caret: 'hide' }));
    out[theme] = path.relative(CONF, file);
    await ctx.close();
  }
  return out;
}

/** Sanity check of a reftest: the stage drawn natively by the feature vs its template. */
async function selfVsRef(browser: Browser, w: Widget, html: string): Promise<Record<string, number>> {
  const res: Record<string, number> = {};
  const reftests = w.stages.filter((s) => s.reftest);
  if (!reftests.length) return res;
  const ctx = await newContext(browser, 'light');
  const page = await openNative(ctx, html, 'native');
  await waitStatus(page, ['done', 'awaiting'], 15_000);
  await page.waitForTimeout(150);
  for (const s of reftests) {
    if (!s.themes.includes('light')) continue;
    const buf = await page.locator(`[data-stage="${s.name}"]`).screenshot({ animations: 'allow', caret: 'hide' });
    const ref = refPath(w, s.name, 'chromium', 'light');
    if (fs.existsSync(ref)) res[s.name] = Math.round(diffStats(PNG.sync.read(buf), PNG.sync.read(fs.readFileSync(ref))).ink * 10000) / 10000;
  }
  await ctx.close();
  return res;
}

const embedded = new Map<string, string>();

function embed(w: Widget, html: string): string {
  return html.replace(/<img\b([^>]*?)\sdata-ref-img="([^"]+)"([^>]*)>/g, (all, a: string, name: string, b: string) => {
    // data-ref-theme="dark": the dark-theme reference of a theme-dependent stage.
    const dark = /\sdata-ref-theme="dark"/.test(a + b);
    const src = embedded.get(`${w.id}#${name}${dark ? '#dark' : ''}`);
    const st = w.stages.find((s) => s.name === name);
    if (!src || !st) return all;
    const rest = (a + b).replace(/\s(src|width|height)="[^"]*"/g, '');
    return `<img${rest} data-ref-img="${name}" width="${st.w}" height="${st.h}" src="${src}">`;
  });
}

function writeIfChanged(file: string, data: string | Buffer) {
  if (fs.existsSync(file) && Buffer.compare(fs.readFileSync(file), Buffer.from(data)) === 0) return;
  fs.writeFileSync(file, data);
}

// ───────────── main ─────────────

async function main() {
  emitCatalogue();
  const ids = new Set(CATALOGUE.map((f) => f.id));
  const widgets = listWidgets(ONLY);
  const browsers: Record<string, Browser> = {};
  for (const e of ENGINES) {
    try { browsers[e] = await (e === 'webkit' ? webkit : chromium).launch(); }
    catch (err) { console.warn(`${e}: not available (${String((err as Error).message).split('\n')[0]}); skipping its references`); }
  }
  if (!browsers.chromium) throw new Error('chromium is required');

  const manifestFile = path.join(CONF, 'manifest.json');
  const prev = readJson<{ tests: any[] }>(manifestFile, { tests: [] });
  const entries = new Map<string, any>(prev.tests.map((t) => [t.id, t]));
  const problems: string[] = [];

  for (const w0 of widgets) {
    let html = withHarness(w0);
    const w = parseWidget(w0.set, w0.stem, html);
    process.stdout.write(`${w.id}: `);
    const refs = NO_REFS ? (entries.get(w.id)?.stages ?? []).reduce((m: any, s: any) => ({ ...m, [s.name]: s.refs }), {}) : await renderRefs(browsers, w, html);
    const refImages = isSdk(w) ? (NO_REFS ? entries.get(w.id)?.refImages ?? {} : await renderSdkRefs(browsers.chromium, w, html)) : undefined;
    const gzip = zlib.gzipSync(Buffer.from(html), { level: 9 }).length;
    if (isSdk(w) && (gzip > MINI_LIMIT.gzip || Buffer.byteLength(html) > MINI_LIMIT.raw)) problems.push(`${w.id}: ${gzip} B gzipped / ${Buffer.byteLength(html)} B, over the mini-widget limit (${MINI_LIMIT.gzip} / ${MINI_LIMIT.raw})`);
    if (w.set === 'visual') html = embed(w, html);
    writeIfChanged(w.file, html);

    const native: Record<string, NativeResult> = {};
    for (const [engine, browser] of Object.entries(browsers)) {
      const ctx = await newContext(browser, 'light');
      native[engine] = await runNative(ctx, w, html);
      await ctx.close();
    }
    const sanity = NO_REFS ? {} : await selfVsRef(browsers.chromium, w, html);

    const checkKeys = new Map<string, { id: string; name: string; native: Record<string, string> }>();
    for (const [engine, r] of Object.entries(native)) {
      for (const [id, name, st] of r.checks) {
        const k = `${id}|${name}`;
        const e = checkKeys.get(k) ?? { id, name, native: {} };
        e.native[engine] = st;
        checkKeys.set(k, e);
      }
    }
    const checks = [...checkKeys.values()];
    const covers = new Set<string>([...checks.map((c) => c.id), ...w.stages.flatMap((s) => s.covers), ...(w.meta.covers ?? []), ...(w.meta.host ?? []).map((h) => h.id)]);
    covers.delete('harness');
    for (const c of covers) if (!ids.has(c)) problems.push(`${w.id}: unknown catalogue id ${c}`);
    for (const c of checks) if (c.id === 'harness') problems.push(`${w.id}: harness error ${c.name}`);
    for (const c of checks) {
      const f = CATALOGUE.find((x) => x.id === c.id);
      if (!isSdk(w) && f?.status === 'in' && c.native.chromium === 'fail') problems.push(`${w.id}: in-profile check fails in native Chromium: ${c.id} — ${c.name}`);
    }
    // Visual-set templates are illustrations (end states, motion diagrams), not reftests: only the auto set is checked.
    if (w.set === 'auto') for (const [s, d] of Object.entries(sanity)) if (d > 0.02 && !w.stages.find((x) => x.name === s)?.xfail) problems.push(`${w.id}: stage ${s} drawn natively differs from its template by ${(d * 100).toFixed(1)} %`);
    const stagePos = (i: number) => (w.set === 'auto' ? [...autoStagePos(i), w.stages[i].w, w.stages[i].h] : null);
    entries.set(w.id, {
      id: w.id,
      set: w.set,
      file: path.relative(CONF, w.file),
      title: w.meta.title,
      hint: w.meta.hint,
      kind: [checks.length ? 'self' : null, w.stages.length ? 'pixel' : null, w.meta.actions?.length ? 'interactive' : null].filter(Boolean),
      covers: [...covers].sort(),
      themes: w.meta.themes ?? (w.stages.some((s) => s.themes.includes('dark')) ? ['light', 'dark'] : ['light']),
      widths: [360, 390, 430],
      actions: w.meta.actions ?? [],
      host: w.meta.host ?? [],
      checks: checks.map((c) => ({ id: c.id, name: c.name, native: c.native })),
      stages: w.stages.map((s, i) => ({ name: s.name, rect: stagePos(i), size: [s.w, s.h], covers: s.covers, themes: s.themes, reftest: s.reftest, maxDiff: s.maxDiff, colors: s.colors, xfail: s.xfail, refs: refs[s.name] ?? {}, nativeVsTemplate: sanity[s.name] })),
      bytes: Buffer.byteLength(html),
      gzip,
      ...(isSdk(w) ? { group: 'sdk', expect: w.meta.expect, contract: w.meta.contract, pending: w.meta.pending, refImages } : {}),
    });
    const n = native.chromium.checks.reduce((m: Record<string, number>, c) => ({ ...m, [c[2]]: (m[c[2]] ?? 0) + 1 }), {});
    console.log(`${checks.length} checks (chromium: ${Object.entries(n).map(([k, v]) => `${k} ${v}`).join(', ')}), ${w.stages.length} stages, ${(Buffer.byteLength(html) / 1024).toFixed(1)} KB (${(gzip / 1024).toFixed(1)} KB gzipped)`);
  }
  for (const b of Object.values(browsers)) await b.close();

  // Drop entries of widgets that no longer exist.
  const all = new Set(listWidgets().map((w) => w.id));
  for (const k of [...entries.keys()]) if (!all.has(k)) entries.delete(k);
  const tests = [...entries.values()].sort((a, b) => a.id.localeCompare(b.id));
  const coverage = (set: string) => new Set(tests.filter((t) => t.set === set).flatMap((t) => t.covers));
  const auto = coverage('auto'), visual = coverage('visual');
  // Covered by other suites of this repository (they kill or flood the widget, which a conformance widget cannot report on itself).
  const external: Record<string, string> = {
    'limit.nodes': 'corpus/attacks (pnpm test:attacks): node flood',
    'limit.cpu': 'corpus/attacks and tests/lifecycle.spec.ts: CPU quota, deferral, kill',
    'limit.memory': 'corpus/attacks (pnpm test:attacks): memory cap',
    'limit.html-size': 'server-side (D20): not a client check',
    // Client behaviour around the widget: judged by people with the client-SDK group (visual/sdk).
    'behaviour.restart': 'visual/sdk/11-crash-restart (client behaviour)',
    'behaviour.gesture-claim': 'visual/sdk/12-drag-gestures (client behaviour)',
    'behaviour.storage-persist': 'visual/sdk/07, 08, 09 (client behaviour)',
    'behaviour.storage-forward': 'visual/sdk/15-storage-forward (client behaviour; waits for the server)',
    'behaviour.message-edit': 'visual/sdk/16-bot-edit (client behaviour; waits for the server)',
    'behaviour.size-hint': 'visual/sdk/17-size-hint (client behaviour; waits for the server)',
  };
  const needAuto = CATALOGUE.filter((f) => f.status !== 'out' && !auto.has(f.id) && !external[f.id]).map((f) => f.id);
  const outUncovered = CATALOGUE.filter((f) => f.status === 'out' && !auto.has(f.id)).map((f) => f.id);
  const manifest = {
    $comment: 'Generated by scripts/conformance/build.ts. One entry per widget of packages/conformance/{auto,visual}; checks[].native = what plain browser engines answer (a sanity reference for the test, not the expectation: out-of-profile and sandbox-only checks differ by design).',
    version: 1,
    catalogue: 'catalogue.json',
    render: { widths: [360, 390, 430], themes: ['light', 'dark'], dpr: DPR, referenceWidth: WIDTH, stageGrid: AUTO_GRID },
    coverage: { auto: auto.size, visual: visual.size, catalogue: CATALOGUE.length, external, notInAuto: needAuto, outNotInAuto: outUncovered },
    tests,
  };
  fs.writeFileSync(manifestFile, JSON.stringify(manifest, null, 1) + '\n');
  // The client-SDK group's post list for the test channel, from the manifest.
  const sdk = tests.filter((t) => t.group === 'sdk');
  if (sdk.length) {
    const lines = [
      '# Client SDK group: what each client implements on its side of SDK v1',
      '',
      'Generated by `scripts/conformance/build.ts` from the widget headers. Contract: `docs/sdk-v1.md`. Each widget is a v1 mini-widget (at most 4 KB gzipped, 32 KB of HTML); post it as a widget message with its picture as a separate image next to it (two pictures: light and dark). A tester does what the instruction says and checks the client against the expected behaviour. Rows marked *waits* need server support first; they are here so the client side can be built against them.',
      '',
      '| # | Widget | Picture | The tester | The client must | Contract | gzip |',
      '|---|---|---|---|---|---|---|',
      ...sdk.map((t) => `| ${t.id.split('/').pop().slice(0, 2)} | [${t.title}](${path.basename(t.file)})${t.pending ? `<br>*waits: ${t.pending}*` : ''} | ${Object.entries(t.refImages ?? {}).map(([k, v]) => `[${k}](../../${v})`).join(' · ')} | ${t.hint ?? ''} | ${t.expect ?? ''} | ${t.contract ?? ''} | ${(t.gzip / 1024).toFixed(1)} KB |`.replace(/\n/g, ' ')),
      '',
    ];
    fs.writeFileSync(path.join(CONF, 'visual', 'sdk', 'README.md'), lines.join('\n'));
  }
  emitCatalogue();
  console.log(`\nmanifest: ${tests.length} widgets; auto covers ${auto.size}, visual ${visual.size} of ${CATALOGUE.length}; in/candidate not in auto: ${needAuto.length}; out not in auto: ${outUncovered.length}`);
  if (needAuto.length) console.log('  not covered by auto: ' + needAuto.join(', '));
  if (problems.length) { console.log('\nproblems:'); for (const p of problems) console.log('  ' + p); }
}

main().catch((e) => { console.error(e); process.exit(1); });
