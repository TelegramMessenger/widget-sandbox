// The bot's profile page (docs/sdk-v1.md §2.19, docs/decisions.md "The bot's profile page", P1-profile): the place
// (`context.surface`, TelegramWidget.surface), the geometry of each place from profile.json `tables.surfaces`, the
// device's safe-area layer (`--tg-safe-area-inset-*`, TelegramWidget.safeAreaInset, safeAreaChanged) next to the
// client's chrome, env() as their sum, the UA layer's padding on four sides, the page height and the phases before and
// after Start through the real sandbox bundle; the last part lays widgets out in the renderer core (headless Chromium).

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';
import { sanitizeStylesheet } from '../../src/host/css-sanitizer.ts';
import { PROFILE_SURFACES, PROFILE_UA_STYLESHEET } from '../../src/shared/profile-tables.ts';
import { REFERENCE_PROFILE, REFERENCE_STYLESHEET, referenceProfileCss } from '../../src/shared/reference.ts';
import {
  THEME_VARIABLES, WIDGET_MESSAGE_KINDS, checkContentSafeArea, checkSafeArea, checkSurface, checkViewportHeight, contentSafeAreaVariables, safeAreaVariables,
  surfaceHeight, surfaceKeepsResizeAnchor, viewportVariables,
} from '../../src/shared/sdk-policy.ts';

const pkgRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const profileJson = JSON.parse(fs.readFileSync(path.join(pkgRoot, 'profile.json'), 'utf8'));

describe('profile page: the place and its geometry', () => {
  it('profile exactly, anything else is a message', () => {
    assert.equal(checkSurface('profile'), 'profile');
    for (const v of [undefined, null, 'message', 'Profile', 'chat', 1, {}]) assert.equal(checkSurface(v), 'message', String(v));
  });

  it('the table is profile.json\'s, with the page ceiling marked provisional', () => {
    assert.deepEqual(PROFILE_SURFACES, profileJson.tables.surfaces);
    assert.deepEqual(PROFILE_SURFACES.message.height, { min: 50, max: 600 });
    assert.deepEqual(profileJson.tables.surfaces.profile.provisional, ['height.max']);
    assert.match(profileJson.decisions['P1-profile'], /provisional/);
  });

  it('a bubble is 50–600 px whatever the viewport; a page at least the viewport, at most the ceiling', () => {
    assert.equal(surfaceHeight('message', 10), 50);
    assert.equal(surfaceHeight('message', 900, 844), 600);
    assert.equal(surfaceHeight('message', 320.5), 320.5);
    const max = PROFILE_SURFACES.profile.height.max;
    assert.equal(surfaceHeight('profile', 300, 844), 844, 'a short document: the page is the viewport');
    assert.equal(surfaceHeight('profile', 2000, 844), 2000);
    assert.equal(surfaceHeight('profile', 50_000, 844), max, 'clipped at the ceiling');
    assert.equal(surfaceHeight('profile', 10), 50, 'without a viewport: the bubble\'s least height');
    assert.equal(surfaceHeight('profile', Number.NaN, 844), 844);
  });

  it('the viewport: a page\'s only, 1…the ceiling, two decimals; a bubble never sets --tg-viewport-height', () => {
    assert.equal(checkViewportHeight('profile', 844.256), 844.26);
    for (const v of [0, -1, Number.NaN, Infinity, '844', PROFILE_SURFACES.profile.height.max + 1]) assert.equal(checkViewportHeight('profile', v), 0, String(v));
    assert.equal(checkViewportHeight('message', 844), 0);
    assert.deepEqual(viewportVariables('profile', 844), { '--tg-viewport-height': '844px' });
    assert.deepEqual(viewportVariables('message', 844), {});
    assert.deepEqual(viewportVariables('profile', 0), {});
  });

  it('the resize anchor moves a bubble, never a page', () => {
    assert.equal(surfaceKeepsResizeAnchor('message'), true);
    assert.equal(surfaceKeepsResizeAnchor(undefined), true);
    assert.equal(surfaceKeepsResizeAnchor('profile'), false);
  });
});

describe('profile page: the two safe-area layers', () => {
  it('the device\'s edges are always 0 in a bubble', () => {
    assert.deepEqual(checkSafeArea({ top: 47, bottom: 34, left: 10, right: 10 }), { top: 0, right: 0, bottom: 0, left: 0 });
    assert.deepEqual(checkSafeArea({ top: 47 }, 'message'), { top: 0, right: 0, bottom: 0, left: 0 });
    assert.deepEqual(safeAreaVariables({ top: 47 }), { '--tg-safe-area-inset-top': '0px', '--tg-safe-area-inset-right': '0px', '--tg-safe-area-inset-bottom': '0px', '--tg-safe-area-inset-left': '0px' });
  });

  it('on a page: any finite number ≥ 0 on every side, as passed (two decimals), with no cap; NaN, infinite, negative or not a number is 0', () => {
    assert.deepEqual(checkSafeArea({ top: 120, right: 40, bottom: 34.125, left: 80.5 }, 'profile'), { top: 120, right: 40, bottom: 34.13, left: 80.5 });
    assert.deepEqual(checkSafeArea({ top: -1, right: Number.NaN, bottom: Infinity, left: '3' }, 'profile'), { top: 0, right: 0, bottom: 0, left: 0 });
    assert.equal(checkSafeArea({ top: 5000 }, 'profile').top, 5000, 'no cap, not even the viewport');
    assert.deepEqual(checkSafeArea(null, 'profile'), { top: 0, right: 0, bottom: 0, left: 0 });
  });

  it('the client\'s chrome on a page: any finite number ≥ 0 on every side, the sides included; the corners 0', () => {
    assert.deepEqual(checkContentSafeArea({ top: 90, bottom: 150, left: 40, right: 12.5, cornerRight: 64, cornerLeft: 1 }, 'profile'),
      { top: 90, right: 12.5, bottom: 150, left: 40, cornerLeft: 0, cornerRight: 0 });
    assert.deepEqual(checkContentSafeArea({ top: -1, bottom: Number.NaN, left: -Infinity, right: '8' }, 'profile'), { top: 0, right: 0, bottom: 0, left: 0, cornerLeft: 0, cornerRight: 0 });
    // A bubble is unchanged: insets 0–64, corners 0–200, over them 0.
    assert.deepEqual(checkContentSafeArea({ bottom: 24, cornerRight: 72, top: 64 }), { top: 64, right: 0, bottom: 24, left: 0, cornerLeft: 0, cornerRight: 72 });
    assert.deepEqual(checkContentSafeArea({ top: 90, bottom: 150, left: 40, cornerRight: 201 }), { top: 0, right: 0, bottom: 0, left: 40, cornerLeft: 0, cornerRight: 0 });
    assert.deepEqual(contentSafeAreaVariables({ cornerRight: 72 }, 'profile')['--tg-content-safe-area-corner-right'], '0px');
    assert.deepEqual(contentSafeAreaVariables({ top: 90, left: 40 }, 'profile')['--tg-content-safe-area-inset-top'], '90px');
  });

  it('the renderer may set every new variable, and the widget may not', () => {
    for (const name of ['--tg-safe-area-inset-top', '--tg-safe-area-inset-right', '--tg-safe-area-inset-bottom', '--tg-safe-area-inset-left', '--tg-viewport-height']) {
      assert.ok(THEME_VARIABLES.includes(name), name);
    }
  });

  it('env(safe-area-inset-*) is the sum of both layers; a unitless 0 fallback becomes 0px', () => {
    const out = (css: string) => sanitizeStylesheet(css, () => {}, { colorScheme: 'light', strict: true });
    assert.equal(out('a{top:env(safe-area-inset-top)}'), 'a{top:calc(var(--tg-safe-area-inset-top,0px) + var(--tg-content-safe-area-inset-top))}');
    assert.equal(out('a{bottom:env(safe-area-inset-bottom, 0)}'), 'a{bottom:calc(var(--tg-safe-area-inset-bottom,0px) + var(--tg-content-safe-area-inset-bottom,0px))}');
    assert.equal(out('a{bottom:env(safe-area-inset-bottom,1.5em)}'), 'a{bottom:calc(var(--tg-safe-area-inset-bottom,0px) + var(--tg-content-safe-area-inset-bottom,1.5em))}');
    assert.equal(out('a{bottom:env(safe-area-inset-bottom,var(--pad, 8px))}'), 'a{bottom:calc(var(--tg-safe-area-inset-bottom,0px) + var(--tg-content-safe-area-inset-bottom,var(--pad,8px)))}');
    // The fallback is still checked: a URL in it drops the declaration.
    assert.equal(out('a{background:env(safe-area-inset-bottom, url(https://example.com/x.png))}'), 'a{}');
  });

  it('the UA layer pads the body by the sum on all four sides; the reference rendering sets both layers to 0', () => {
    for (const side of ['top', 'right', 'bottom', 'left']) {
      assert.ok(PROFILE_UA_STYLESHEET.includes(`calc(var(--tg-safe-area-inset-${side},0px) + var(--tg-content-safe-area-inset-${side},0px))`), side);
      assert.ok(REFERENCE_STYLESHEET.includes(`--tg-safe-area-inset-${side}:0px`), side);
    }
    assert.ok(!PROFILE_UA_STYLESHEET.includes('padding-bottom:var(--tg-content-safe-area-inset-bottom'), 'the old bottom-only rule is gone');
    const page = referenceProfileCss();
    assert.ok(page.includes('--tg-safe-area-inset-top:47px') && page.includes('--tg-content-safe-area-inset-bottom:64px') && page.includes('--tg-viewport-height:844px'), page);
  });
});

describe('profile page: TelegramWidget in the sandbox', () => {
  const probes = ['TelegramWidget.surface', 'JSON.stringify(TelegramWidget.safeAreaInset)', 'Object.isFrozen(TelegramWidget.safeAreaInset)', 'JSON.stringify(TelegramWidget.contentSafeAreaInset)'];

  it('a message by default: surface message, the device\'s edges 0 whatever the client passed', async () => {
    const r = await runWidget('<p>x</p>', { safeArea: { top: 47, bottom: 34 }, contentSafeArea: { bottom: 24, cornerRight: 64 }, probes });
    assert.deepEqual(r.probes, ['message', '{"top":0,"right":0,"bottom":0,"left":0}', 'true', '{"top":0,"right":0,"bottom":24,"left":0,"cornerLeft":0,"cornerRight":64}']);
    assert.equal(r.surface, 'message');
  });

  it('a profile page: both layers as passed at boot, past the old ranges; a negative side and the corners 0', async () => {
    const r = await runWidget('<p>x</p>', { context: { surface: 'profile' }, safeArea: { top: 120, bottom: 34, left: 99, right: -3 }, contentSafeArea: { top: 90, bottom: 150, left: 40, cornerRight: 64 }, probes });
    assert.deepEqual(r.probes, ['profile', '{"top":120,"right":0,"bottom":34,"left":99}', 'true', '{"top":90,"right":0,"bottom":150,"left":40,"cornerLeft":0,"cornerRight":0}']);
  });

  it('safeAreaChanged: one event and a fresh frozen object per change; the same value fires nothing; a change drops cached layout answers', async () => {
    const r = await runWidget(`<div id="d">x</div><script>
      var log = [], seen = [], d = document.getElementById('d');
      TelegramWidget.onEvent('safeAreaChanged', function () { var a = TelegramWidget.safeAreaInset; seen.push(a); log.push([a.top, a.bottom, a.left, Object.isFrozen(a), this === TelegramWidget].join(':')); });
      TelegramWidget.onEvent('contentSafeAreaChanged', function () { log.push('content'); });
    </script>`, {
      context: { surface: 'profile' }, safeArea: { top: 47, bottom: 34 },
      script: [
        { probe: 'd.getBoundingClientRect().width' },
        { deviceSafeArea: { top: 47, bottom: 34 } },
        { deviceSafeArea: { top: 0, bottom: 21, left: 59 } },
        { deviceSafeArea: { top: 0, bottom: 21, left: 59 } },
        { probe: 'd.getBoundingClientRect().width' },
        { probe: 'log.join("|") + "|" + (seen.length === 1)' },
      ],
    });
    assert.equal(r.probes[2], '0:21:59:true:true|true');
    assert.deepEqual(r.safeArea, { top: 0, right: 0, bottom: 21, left: 59 });
    const rects = r.queries.filter((q) => JSON.stringify(q).includes('rect'));
    assert.equal(rects.length, 2, 'the probe after the change is not a cache hit: ' + JSON.stringify(r.queries));
  });

  it('in a bubble a device change is 0 and fires nothing', async () => {
    const r = await runWidget(`<script>var n = 0; TelegramWidget.onEvent('safeAreaChanged', function () { n++; });</script>`, {
      script: [{ deviceSafeArea: { top: 47 } }, { probe: 'n + JSON.stringify(TelegramWidget.safeAreaInset)' }],
    });
    assert.deepEqual(r.probes, ['0{"top":0,"right":0,"bottom":0,"left":0}']);
  });

  it('setResizeAnchor is checked on a page but reaches no host: the page never moves', async () => {
    const html = `<script>TelegramWidget.setResizeAnchor('bottom'); try { TelegramWidget.setResizeAnchor('middle'); } catch (e) { window.bad = e instanceof TypeError; }</script>`;
    const page = await runWidget(html, { context: { surface: 'profile' }, probes: ['window.bad'] });
    assert.deepEqual(page.probes, ['true']);
    assert.deepEqual(page.resizeAnchors, []);
    assert.equal(page.resizeAnchor, 'top');
    assert.ok(!page.bridge.some((b) => JSON.stringify(b).includes('setResizeAnchor')), JSON.stringify(page.bridge));
    const bubble = await runWidget(html);
    assert.deepEqual(bubble.resizeAnchors, ['bottom']);
  });
});

describe('profile page: before and after Start', () => {
  const viewer = { id: 1001, firstName: 'Ann' };
  const idOf = async (html: string, tag: string) => ((await runWidget(html)).renderOps.find((o: any) => o.op === 'el' && o.tag === tag) as any).id as number;
  const tap = async () => [{ type: 'click' as const, targetId: await idOf(tapRequest, 'button'), gesture: 1 }];
  const probes = ['TelegramWidget.surface', 'String(TelegramWidget.chatType)', 'JSON.stringify(TelegramWidget.viewer)', 'TelegramWidget.isForward', 'String(TelegramWidget.wallet)'];
  const tapRequest = `<button id="b">go</button><script>window.out = [];
    document.getElementById('b').addEventListener('click', function () { TelegramWidget.request('tap').then(function (r) { out.push('ok:' + r.result); }, function (e) { out.push('tap:' + e.code); }); });
    TelegramWidget.request('bg').then(function () { out.push('bg:sent'); }, function (e) { out.push('bg:' + e.code); });
  </script>`;
  const ctx = (kind: keyof typeof WIDGET_MESSAGE_KINDS) => ({ ...WIDGET_MESSAGE_KINDS[kind], viewer, wallet: true });

  it('the kinds: before Start no flags, after Start requests on a tap and in the background, never a chat type', () => {
    assert.deepEqual(WIDGET_MESSAGE_KINDS['profile-prestart'], { isCopy: false, canQuery: false, backgroundQuery: false, chatType: null, surface: 'profile' });
    assert.deepEqual(WIDGET_MESSAGE_KINDS.profile, { isCopy: false, canQuery: true, backgroundQuery: true, chatType: null, surface: 'profile' });
    assert.deepEqual(Object.keys(WIDGET_MESSAGE_KINDS).filter((k) => k.startsWith('profile')), ['profile-prestart', 'profile']);
  });

  it('before Start: no requests, no viewer, no chat type, no wallet, whatever the client passes besides the flags', async () => {
    const r = await runWidget(tapRequest, { context: { ...ctx('profile-prestart'), chatType: 'private' }, onRequest: () => ({ result: 'x' }), events: await tap(), probes: [...probes, 'out.join()'] });
    assert.deepEqual(r.probes.slice(0, 5), ['profile', 'null', 'null', 'false', 'null']);
    assert.deepEqual(r.requests, [], 'nothing reaches the client');
    assert.match(r.probes[5] as string, /^(bg|tap):[A-Z_]+,(bg|tap):[A-Z_]+$/, 'both refused in the widget: ' + r.probes[5]);
  });

  it('after Start: background requests, the viewer and (where the client has it) the wallet, but no chat type even when the client passes one', async () => {
    const r = await runWidget(tapRequest, { context: { ...ctx('profile'), chatType: 'private' }, onRequest: () => ({ result: 'x' }), events: await tap(), probes: [...probes.slice(0, 4), 'TelegramWidget.wallet !== null', 'out.join()'] });
    assert.deepEqual(r.probes, ['profile', 'null', '{"id":1001,"first_name":"Ann"}', 'false', 'true', 'bg:sent,ok:x']);
    assert.deepEqual(r.requests.map((q) => q.verdict), ['background', 'foreground']);
  });

});

describe('profile page: the renderer core lays it out (headless Chromium)', () => {
  const layout = (html: string, opts: Parameters<typeof runWidget>[1] = {}) => runWidget(html, { width: REFERENCE_PROFILE.width, layout: true, ...opts });
  const page = { context: { surface: 'profile' as const }, safeArea: REFERENCE_PROFILE.safeArea, contentSafeArea: REFERENCE_PROFILE.contentSafeArea, viewportHeight: REFERENCE_PROFILE.viewportHeight };

  it('the UA layer pads an unstyled body by both layers, top and bottom', async () => {
    const plain = '<style>body{margin:0}</style><div style="height:10px"></div>';
    assert.equal(Math.round((await layout(plain, page)).height!), 10 + 47 + 56 + 34 + 64);
    assert.equal(Math.round((await layout(plain)).height!), 10, 'a bubble without a safe area: as before');
  });

  it('env() resolves to the sum, the variables to each layer, --tg-viewport-height to the viewport', async () => {
    const box = (css: string) => `<style>body{margin:0;padding:0}</style><div style="${css}"></div>`;
    assert.equal(Math.round((await layout(box('height:env(safe-area-inset-top)'), page)).height!), 103);
    assert.equal(Math.round((await layout(box('height:env(safe-area-inset-bottom, 7px)'), page)).height!), 98);
    assert.equal(Math.round((await layout(box('height:var(--tg-safe-area-inset-top)'), page)).height!), 47);
    assert.equal(Math.round((await layout(box('height:var(--tg-viewport-height, 20px)'), page)).height!), 844);
    // Past the old ranges (a large system font, a foldable): as passed.
    const tall = { ...page, safeArea: { top: 120, left: 40 }, contentSafeArea: { top: 90, bottom: 150, left: 40 } };
    assert.equal(Math.round((await layout(box('height:env(safe-area-inset-top)'), tall)).height!), 210);
    assert.equal(Math.round((await layout(box('height:env(safe-area-inset-left)'), tall)).height!), 80);
    // In a bubble the device's layer is 0 and --tg-viewport-height is not set: the fallback applies.
    assert.equal(Math.round((await layout(box('height:var(--tg-viewport-height, 20px)'), { safeArea: { top: 47 } })).height!), 20);
    assert.equal(Math.round((await layout(box('height:calc(5px + env(safe-area-inset-top))'), { safeArea: { top: 47 } })).height!), 5);
  });

  it('the widget cannot replace the device\'s variables', async () => {
    const html = '<style>:root{--tg-safe-area-inset-top:50px}body{margin:0;padding:0}</style><div style="height:var(--tg-safe-area-inset-top)"></div>'
      + '<script>document.documentElement.style.setProperty("--tg-safe-area-inset-top", "70px")</script>';
    assert.equal(Math.round((await layout(html, page)).height!), 47);
  });
});
