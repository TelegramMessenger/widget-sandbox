// Web runner of the conformance set: every widget of corpus/conformance on the
// web dev app (apps/web) through the real pipeline (QuickJS worker → validator
// → renderer), on each renderer: iframe, shadow (Shadow DOM, the web product
// renderer), blitz and litehtml (wasm, when built). For each widget it
//   - reads the result from the validated ops: it taps the renderer's `send`
//     and keeps the last `data-conformance` attribute set on the element the
//     `el` op adopted as <html> (what any host can do with its own ops);
//   - performs the widget's scripted actions (taps, drags, hover, theme) at the
//     target rects the widget published, then waits for the result again;
//   - screenshots the bubble at DPR 2, crops each stage of the auto set and
//     diffs it against the Chromium/WebKit references;
//   - checks the host-side expectations (bridge calls, height).
// Output: reports/conformance/<renderer>/<widget>.json (+ stage crops and diffs)
// and the per-renderer report reports/conformance/<renderer>.json; report.ts
// turns those into corpus/conformance/results/.
//
// Needs the dev server of this checkout:  corepack pnpm dev --port 5175
//   corepack pnpm tsx scripts/conformance/run-web.ts --origin http://127.0.0.1:5175 \
//     [--renderers iframe,shadow,blitz,litehtml] [--set auto,visual] [--only 03-layout] \
//     [--themes light,dark] [--widths 390] [--features webAnimations] [--policy strict]

import fs from 'node:fs';
import path from 'node:path';
import { chromium, type Page } from '@playwright/test';
import { PNG } from 'pngjs';
import { DIFF, ROOT, autoStagePos, colorsMatch, crop, diffRatio, listWidgets, perform, refPath, type Published, type Widget } from './lib';

const args = process.argv.slice(2);
const opt = (name: string, def?: string) => (args.includes('--' + name) ? args[args.indexOf('--' + name) + 1] : def);
const ORIGIN = opt('origin', `http://127.0.0.1:${process.env.TW_PORT ?? 5175}`)!;
const RENDERERS = opt('renderers', 'iframe,shadow,blitz,litehtml')!.split(',');
const SETS = opt('set', 'auto,visual')!.split(',');
const THEMES = opt('themes');
const WIDTHS = opt('widths', '390')!.split(',').map(Number);
const FEATURES = opt('features', 'webAnimations')!;
const POLICY = opt('policy', 'strict')!;
const ONLY = opt('only');
const DPR = 2;
const OUT = path.join(ROOT, 'reports', 'conformance');

export interface StageResult { name: string; covers: string[]; diff: number | null; ref: string | null; maxDiff: number; status: 'pass' | 'fail' | 'xfail' | 'xpass' | 'skip'; detail?: string }
export interface HostResult { id: string; name: string; status: string; detail: string }
export interface WidgetRun {
  id: string; set: string; renderer: string; theme: string; width: number;
  loaded: boolean; dead: string | null; status: string; ms: number;
  checks: [string, string, string, string][];
  stages: StageResult[]; host: HostResult[];
  skippedActions: string[]; errors: string[]; diags: Record<string, number>; height: number;
  /** Stage rects as the widget measured them in this renderer (CSS px). */
  stageRects?: Record<string, number[]>;
}

const canvasRenderer = (r: string) => r === 'blitz' || r === 'litehtml';

async function setup(page: Page, renderer: string, theme: string) {
  const q = new URLSearchParams({ theme, features: FEATURES, policy: POLICY });
  if (renderer !== 'iframe') q.set(renderer, '1');
  await page.goto(`${ORIGIN}/?${q}`);
  await page.waitForFunction(() => !!(window as any).__tw, null, { timeout: 30_000 });
}

/** Load the widget into the dev app and tap the renderer's ops for the result attribute. */
async function load(page: Page, w: Widget, width: number) {
  await page.evaluate(({ html, name, width }) => {
    const wrap = document.querySelector('#sandbox-wrap') as HTMLElement;
    const chat = wrap.closest('.chat') as HTMLElement;
    const bubble = wrap.closest('.bubble') as HTMLElement;
    chat.style.width = width + 'px';
    bubble.style.padding = '0';
    bubble.style.borderRadius = '0';
    bubble.style.boxShadow = 'none';
    const tw = (window as any).__tw;
    const conf: any = ((window as any).__conf = { html: null as number | null, attr: null as string | null, ops: 0 });
    tw.load(html, name);
    tw.host.element.setAttribute('data-conf-el', '');
    const r = tw.host.renderer;
    const send = r.send.bind(r);
    r.send = (m: any) => {
      if (m && m.type === 'ops') {
        for (const op of m.ops) {
          conf.ops++;
          if (op.op === 'el' && op.adopt === 'html') {
            conf.html = op.id;
            for (const [n, v] of op.attrs) if (n === 'data-conformance') conf.attr = v;
          } else if (op.op === 'attr' && op.id === conf.html && op.name === 'data-conformance') conf.attr = op.value;
        }
      }
      return send(m);
    };
  }, { html: w.html, name: `conformance/${w.id}.html`, width });
}

async function published(page: Page): Promise<Published | null> {
  return page.evaluate(() => { const a = (window as any).__conf?.attr; try { return a ? JSON.parse(a) : null; } catch { return null; } });
}

async function waitStatus(page: Page, want: string[], timeout: number): Promise<Published | null> {
  const t0 = Date.now();
  let last: Published | null = null;
  while (Date.now() - t0 < timeout) {
    last = await published(page);
    if (last && want.includes(last.status)) return last;
    const dead = await page.evaluate(() => (window as any).__tw.state.dead);
    if (dead) return last;
    await page.waitForTimeout(100);
  }
  return last;
}

async function runWidget(page: Page, w: Widget, renderer: string, theme: 'light' | 'dark', width: number): Promise<WidgetRun> {
  const t0 = Date.now();
  await load(page, w, width);
  await page.waitForFunction(() => { const s = (window as any).__tw.state; return s.loaded || s.dead; }, null, { timeout: 60_000 }).catch(() => {});
  let pub = await waitStatus(page, ['done', 'awaiting'], 20_000);
  let skippedActions: string[] = [];
  let skippedIds: string[] = [];
  if (w.meta.actions?.length && pub) {
    const wrap = page.locator('[data-conf-el]');
    ({ skipped: skippedActions, ids: skippedIds } = await perform(page, w.meta.actions, {
      origin: async () => { const b = await wrap.boundingBox(); return { x: b?.x ?? 0, y: b?.y ?? 0 }; },
      setTheme: async (t) => { await page.evaluate((t) => (window as any).__tw.setTheme(t), t); },
      setVisible: async (v) => { await page.evaluate((v) => (window as any).__tw.setVisible(v), v); },
      tapOnly: canvasRenderer(renderer) ? async (x, y) => { await page.evaluate(([x, y]) => (window as any).__tw.tap(x, y), [x, y]); } : undefined,
    }, () => published(page)));
    pub = await waitStatus(page, ['done'], skippedActions.length ? 3_000 : 10_000);
  }
  // Checks fed by input this surface cannot deliver (canvas renderers: taps only) are not judged.
  const checks = (pub?.checks ?? []).map((c) => (skippedIds.includes(c[0]) ? [c[0], c[1], 'skip', 'input not deliverable on this renderer: ' + skippedActions.join(', ')] : c) as [string, string, string, string]);
  if (canvasRenderer(renderer)) await page.waitForFunction((r) => ((window as any).__tw[r]?.paints ?? 0) > 0, renderer, { timeout: 15_000 }).catch(() => {});
  await page.waitForTimeout(250);
  const state = await page.evaluate(() => { const s = (window as any).__tw.state; return { loaded: s.loaded, dead: s.dead, diags: s.diags, logs: s.logs, bridge: s.bridge, height: s.height }; });

  const dir = path.join(OUT, renderer, w.set);
  fs.mkdirSync(dir, { recursive: true });
  const tag = `${w.stem}.${theme}.${width}`;
  const stages: StageResult[] = [];
  if (w.set === 'auto' && w.stages.length) {
    // The renderer element can sit at a fractional CSS position; its content is snapped to device pixels,
    // an element screenshot is clipped at whole CSS pixels: crop from the viewport at the snapped origin.
    const box = await page.locator('[data-conf-el]').boundingBox();
    const shot = PNG.sync.read(await page.screenshot({ animations: 'allow', caret: 'hide' }));
    const ox = Math.round((box?.x ?? 0) * DPR), oy = Math.round((box?.y ?? 0) * DPR);
    w.stages.forEach((s, i) => {
      const covers = s.covers;
      const maxDiff = s.maxDiff ?? DIFF.maxRatio;
      if (!s.themes.includes(theme)) return;
      const pos = autoStagePos(i);
      const got = crop(shot, pos[0], pos[1], s.w, s.h, DPR, ox, oy);
      fs.writeFileSync(path.join(dir, `${tag}.${s.name}.png`), PNG.sync.write(got));
      let best: { diff: number; ref: string; colors?: { ok: boolean; detail: string } } | null = null;
      for (const engine of ['chromium', 'webkit']) {
        const ref = refPath(w, s.name, engine, theme);
        if (!fs.existsSync(ref)) continue;
        const refPng = PNG.sync.read(fs.readFileSync(ref));
        const d = diffRatio(got, refPng, path.join(dir, `${tag}.${s.name}.${engine}.diff.png`));
        const colors = s.colors ? colorsMatch(got, refPng, s.colors) : undefined;
        const score = d + (colors && !colors.ok ? 1 : 0);
        if (!best || score < best.diff + (best.colors && !best.colors.ok ? 1 : 0)) best = { diff: d, ref: engine, colors };
      }
      const ok = best !== null && best.diff <= maxDiff && (!best.colors || best.colors.ok);
      // A stage that shows the effect of input this renderer cannot receive (a drag) is not judged.
      const undeliverable = covers.some((c) => skippedIds.includes(c));
      const status = best === null || undeliverable ? 'skip' : s.xfail ? (ok ? 'xpass' : 'xfail') : ok ? 'pass' : 'fail';
      stages.push({ name: s.name, covers, diff: best ? Math.round(best.diff * 10000) / 10000 : null, ref: best?.ref ?? null, maxDiff, status, detail: [best?.colors?.detail, s.xfail].filter(Boolean).join('; ') || undefined });
    });
  }

  const host: HostResult[] = [];
  for (const h of w.meta.host ?? []) {
    if (h.gesture && canvasRenderer(renderer)) { host.push({ id: h.id, name: h.name, status: 'skip', detail: 'synthetic taps grant no user gesture' }); continue; }
    let ok = true;
    let detail = '';
    if (h.bridge) {
      const calls = (state.bridge as { method: string; args: unknown[] }[]).filter((b) => b.method === h.bridge!.method && (h.bridge!.arg === undefined || b.args[0] === h.bridge!.arg));
      ok &&= calls.length === h.bridge.count;
      detail += `${h.bridge.method} ×${calls.length} (want ${h.bridge.count})`;
    }
    if (h.height) {
      ok &&= (h.height.min === undefined || state.height >= h.height.min) && (h.height.max === undefined || state.height <= h.height.max);
      detail += `${detail ? '; ' : ''}height ${state.height}`;
    }
    host.push({ id: h.id, name: h.name, status: h.xfail ? (ok ? 'xpass' : 'xfail') : ok ? 'pass' : 'fail', detail });
  }

  const run: WidgetRun = {
    id: w.id, set: w.set, renderer, theme, width,
    loaded: !!state.loaded && !state.dead, dead: state.dead, status: pub?.status ?? 'no result', ms: Date.now() - t0,
    checks, stages, host, skippedActions,
    errors: (state.logs as { level: string; text: string }[]).filter((l) => l.level === 'error').map((l) => String(l.text).split('\n')[0].slice(0, 200)),
    diags: Object.fromEntries(Object.entries(state.diags as Record<string, { count: number }>).map(([k, v]) => [k, v.count])),
    height: state.height,
    stageRects: pub?.stages,
  };
  fs.writeFileSync(path.join(dir, `${tag}.json`), JSON.stringify(run, null, 1));
  return run;
}

async function main() {
  const widgets = listWidgets(ONLY).filter((w) => SETS.includes(w.set));
  const browser = await chromium.launch();
  for (const renderer of RENDERERS) {
    const ctx = await browser.newContext({ viewport: { width: 1400, height: 1100 }, deviceScaleFactor: DPR });
    const page = await ctx.newPage();
    const runs: WidgetRun[] = [];
    // --themes widens the default; a widget that declares its themes (a scenario that starts in light) keeps to them.
    const themes = (w: Widget) => (w.meta.themes ?? (THEMES ? THEMES.split(',') : w.stages.some((s) => s.themes.includes('dark')) ? ['light', 'dark'] : ['light'])) as ('light' | 'dark')[];
    let current = '';
    let available = true;
    for (const w of widgets) {
      for (const theme of themes(w)) {
        for (const width of WIDTHS) {
          if (current !== theme) { await setup(page, renderer, theme); current = theme; }
          try {
            const r = await runWidget(page, w, renderer, theme, width);
            if (canvasRenderer(renderer) && r.diags[`${renderer}-init`] && !r.loaded) available = false;
            runs.push(r);
            const n = r.checks.reduce((m: Record<string, number>, c) => ({ ...m, [c[2]]: (m[c[2]] ?? 0) + 1 }), {});
            const st = r.stages.map((s) => `${s.name} ${s.diff === null ? '?' : (s.diff * 100).toFixed(1) + '%'}${s.status === 'pass' ? '' : ' ' + s.status}`).join(', ');
            console.log(`${renderer.padEnd(8)} ${w.id.padEnd(28)} ${theme.padEnd(5)} ${width} ${r.status.padEnd(8)} ${Object.entries(n).map(([k, v]) => `${k} ${v}`).join(' ')}${st ? ' | ' + st : ''}${r.dead ? ' DEAD ' + r.dead.slice(0, 80) : ''}${r.errors.length ? ` | ${r.errors.length} errors` : ''}`);
          } catch (e) {
            console.log(`${renderer} ${w.id} ${theme} ${width}: runner error ${(e as Error).message.split('\n')[0]}`);
            current = '';
          }
        }
      }
      if (!available) { console.log(`${renderer}: not available (wasm not built?), skipping the rest`); break; }
    }
    fs.mkdirSync(OUT, { recursive: true });
    // A partial run (--only, --set) updates its entries and keeps the rest of the renderer's last run.
    const prevFile = path.join(OUT, `${renderer}.json`);
    if (fs.existsSync(prevFile)) {
      const key = (r: WidgetRun) => `${r.id}|${r.theme}|${r.width}`;
      const fresh = new Set(runs.map(key));
      const prev = (JSON.parse(fs.readFileSync(prevFile, 'utf8')).runs ?? []) as WidgetRun[];
      runs.unshift(...prev.filter((r) => !fresh.has(key(r))));
      runs.sort((a, b) => key(a).localeCompare(key(b)));
    }
    fs.writeFileSync(path.join(OUT, `${renderer}.json`), JSON.stringify({ renderer, platform: 'web', engine: `Chromium ${browser.version()} (headless, Playwright)`, origin: ORIGIN, features: FEATURES.split(',').filter(Boolean), policy: POLICY, date: new Date().toISOString(), runs }, null, 1));
    await ctx.close();
  }
  await browser.close();
}

main().catch((e) => { console.error(e); process.exit(1); });
