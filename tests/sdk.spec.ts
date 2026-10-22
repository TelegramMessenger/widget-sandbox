// SDK v1 on the web host (docs/sdk-v1.md): the theme variables on the
// renderer root, gesture claims, Web Storage across a restart, the client
// locale and the actions a tap produces, on the iframe and the Shadow DOM
// renderer. The widget-facing API itself is unit-tested through QuickJS
// (packages/core/tests/unit/sdk.test.ts).

import { expect, test, type Locator, type Page } from '@playwright/test';
import { PNG } from 'pngjs';

test.skip(process.env.VITE_TW_POLICY === 'compat', 'user activation belongs to the strict policy');

const tw = (page: Page) => page.evaluate(() => (window as any).__tw.state);

async function load(page: Page, html: string, opts: { shadow?: boolean; name?: string | null; query?: string } = {}) {
  await page.goto('/' + (opts.query ?? (opts.shadow ? '?shadow=1' : '')));
  await page.evaluate(([h, n]) => (window as any).__tw.load(h, n), [html, opts.name ?? null] as const);
  await page.waitForFunction(() => (window as any).__tw.state.loaded);
  await page.waitForTimeout(100);
  return opts.shadow ? page.locator('#sandbox-wrap .widget-shadow') : page.frameLocator('#sandbox-wrap iframe').locator(':root');
}
const css = (l: Locator, prop: string) => l.evaluate((e, p) => getComputedStyle(e).getPropertyValue(p).trim(), prop);

const THEME_WIDGET = `<style>
  :root { --tg-theme-bg-color: #ff0000 !important; }
  html:root:root { --tg-theme-text-color: #ff0000 !important; }
  #inner { --tg-theme-link-color: #123456; }
</style>
<div id="probe">probe</div><div id="inner"><span id="shadowed">x</span></div><p id="seen"></p>
<script>
  document.documentElement.style.setProperty('--tg-theme-hint-color', '#00ff00', 'important');
  function show() { document.getElementById('seen').textContent = TelegramWidget.colorScheme + ' ' + TelegramWidget.themeParams.bg_color; }
  TelegramWidget.onEvent('themeChanged', show);
  show();
</script>`;

for (const shadow of [false, true]) {
  const renderer = shadow ? 'shadow' : 'iframe';

  test(`${renderer}: the theme variables are on the root, the widget cannot override them there, only shadow them`, async ({ page }) => {
    const root = await load(page, THEME_WIDGET, { shadow });
    const probe = root.locator('#probe');
    expect(await css(probe, '--tg-theme-bg-color')).toBe('#ffffff');
    expect(await css(probe, '--tg-theme-text-color')).toBe('#000000');
    expect(await css(probe, '--tg-theme-hint-color')).toBe('#707579');
    expect(await css(probe, '--tg-color-scheme')).toBe('light');
    expect(await css(root.locator('#shadowed'), '--tg-theme-link-color')).toBe('#123456');
    await expect(root.locator('#seen')).toHaveText('light #ffffff');

    await page.evaluate(() => (window as any).__tw.setTheme('dark'));
    await expect(root.locator('#seen')).toHaveText('dark #212121');
    expect(await css(probe, '--tg-theme-bg-color')).toBe('#212121');
    expect(await css(probe, '--tg-color-scheme')).toBe('dark');
  });

  test(`${renderer}: a theme with params and button tokens reaches the widget and the root`, async ({ page }) => {
    const root = await load(page, THEME_WIDGET, { shadow });
    await page.evaluate(() => (window as any).__tw.host.setTheme({ colorScheme: 'light', params: { bg_color: '#FAFAFA', button_color: 'blue' }, button: { radius: '12px', font: '15px Helvetica' } }));
    await expect(root.locator('#seen')).toHaveText('light #fafafa');
    const probe = root.locator('#probe');
    expect(await css(probe, '--tg-theme-bg-color')).toBe('#fafafa');
    expect(await css(probe, '--tg-theme-button-color')).toBe('#3390ec');
    expect(await css(probe, '--tg-button-radius')).toBe('12px');
    expect(await css(probe, '--tg-button-font')).toBe('');
  });

  test(`${renderer}: the UA layer draws an unstyled widget on the text tokens, the bubble behind it; author CSS wins`, async ({ page }) => {
    const root = await load(page, `<h1 id="h">Title</h1><p id="p">Text <a id="a" href="https://telegram.org">link</a></p><ul id="ul"><li>x</li></ul>`, { shadow });
    const html = shadow ? root.locator('html') : root;
    const body = root.locator('body');
    const h = root.locator('#h');
    // no background of its own: the bubble behind the transparent renderer is the canvas
    expect(await css(html, 'background-color')).toBe('rgba(0, 0, 0, 0)');
    expect(await css(html, 'color')).toBe('rgb(0, 0, 0)');
    expect([await css(html, 'font-size'), await css(html, 'line-height')]).toEqual(['15px', '20.25px']);
    expect([await css(body, 'margin'), await css(body, 'padding')]).toEqual(['8px 12px', '0px']);
    expect([await css(h, 'font-size'), await css(h, 'margin-top'), await css(h, 'font-weight')]).toEqual(['24px', '0px', '600']);
    expect(await css(root.locator('#a'), 'color')).toBe('rgb(51, 144, 236)');
    expect(await css(root.locator('#ul'), 'padding-left')).toBe('22.5px');
    expect(await css(h, '--tg-text-font-size')).toBe('15px');
    const height = (await tw(page)).height;

    await page.evaluate(() => (window as any).__tw.host.setTheme({ colorScheme: 'dark', params: { bg_color: '#101820' }, text: { fontSize: 18, lineHeight: 1.5, padding: '4px 6px' } }));
    await expect.poll(() => css(html, 'font-size')).toBe('18px');
    expect(await css(html, '--tg-theme-bg-color')).toBe('#101820');
    expect(await css(html, 'background-color')).toBe('rgba(0, 0, 0, 0)');
    expect(await css(html, 'color')).toBe('rgb(255, 255, 255)');
    expect(await css(html, 'line-height')).toBe('27px');
    expect(await css(body, 'margin')).toBe('4px 6px');
    expect(await css(h, 'font-size')).toBe('28.8px');
    await expect.poll(async () => (await tw(page)).height).toBeGreaterThan(height);

    await page.evaluate(() => (window as any).__tw.host.setTheme({ colorScheme: 'light', text: { fontSize: 100, lineHeight: '2em', padding: 'auto' } }));
    await expect.poll(() => css(html, 'font-size')).toBe('15px');
    expect([await css(html, 'line-height'), await css(body, 'margin')]).toEqual(['20.25px', '8px 12px']);

    const styled = await load(page, `<style>body { margin: 0 } h1 { margin-top: 5px; font-size: 10px } a { color: #ff0000 }</style><h1 id="h">T</h1><a id="a" href="https://telegram.org">l</a>`, { shadow });
    expect(await css(styled.locator('body'), 'margin')).toBe('0px');
    expect([await css(styled.locator('#h'), 'margin-top'), await css(styled.locator('#h'), 'font-size')]).toEqual(['5px', '10px']);
    expect(await css(styled.locator('#a'), 'color')).toBe('rgb(255, 0, 0)');
  });

  test(`${renderer}: a body background reaches the whole bubble, its margins included (root background propagation)`, async ({ page }) => {
    await load(page, `<style>body { background: #2b5278; color: #fff }</style><p>Body background, no margin reset.</p>`, { shadow });
    const frame = page.locator('#sandbox-wrap .widget-frame');
    const box = (await frame.boundingBox())!;
    // pixels in the body's margins (12 px at the sides, 8 px at the top), away from the bubble's rounded corners
    const shot = await page.screenshot({ clip: { x: box.x, y: box.y, width: box.width, height: Math.min(box.height, 40) } });
    const png = PNG.sync.read(shot);
    const at = (x: number, y: number) => { const i = (y * png.width + x) * 4; return [png.data[i], png.data[i + 1], png.data[i + 2]]; };
    const scale = png.width / box.width;
    for (const [x, y] of [[3, 20], [Math.round(box.width / 2), 3], [Math.round(box.width) - 3, 20]]) expect(at(Math.round(x * scale), Math.round(y * scale)), `${x},${y}`).toEqual([0x2b, 0x52, 0x78]);
  });

  test(`${renderer}: the demo bubble is the canvas: a body background fills it to the rounded corners (visual/15-body-background)`, async ({ page }) => {
    for (const theme of ['light', 'dark'] as const) {
      const q = new URLSearchParams({ file: 'conformance/visual/15-body-background.html', ...(shadow ? { shadow: '1' } : {}), ...(theme === 'dark' ? { theme: 'dark' } : {}) });
      await page.goto('/?' + q);
      await page.waitForFunction(() => (window as any).__tw.state.loaded);
      await page.waitForTimeout(300);
      // The whole bubble the dev app draws, not only the renderer's element: a padded bubble would leave a strip of its own colour.
      const bubble = page.locator('#sandbox-wrap').locator('xpath=..');
      const png = PNG.sync.read(await bubble.screenshot());
      const at = (x: number, y: number) => { const i = (y * png.width + x) * 4; return [png.data[i], png.data[i + 1], png.data[i + 2]]; };
      const w = png.width, h = png.height, s = Math.round(png.width / ((await bubble.boundingBox())!.width));
      const blue = [0x2b, 0x52, 0x78];
      // just inside the 14 px radius at each corner, and the middle of each edge
      for (const [x, y] of [[6, 6], [w / s - 7, 6], [6, h / s - 7], [w / s - 7, h / s - 7], [w / s / 2, 1], [1, h / s / 2], [w / s - 2, h / s / 2], [w / s / 2, h / s - 2]]) {
        expect(at(Math.round(x * s), Math.round(y * s)), `${theme} ${x},${y}`).toEqual(blue);
      }
      // the very corner is outside the radius: clipped, not blue
      expect(at(0, 0), `${theme} corner`).not.toEqual(blue);
    }
  });

  test(`${renderer}: an unstyled widget sits on the bubble's own colour in both themes (the renderer stays transparent)`, async ({ page }) => {
    for (const theme of ['light', 'dark'] as const) {
      const q = new URLSearchParams({ file: 'defaults/static/01-text-card.html', ...(shadow ? { shadow: '1' } : {}), ...(theme === 'dark' ? { theme: 'dark' } : {}) });
      await page.goto('/?' + q);
      await page.waitForFunction(() => (window as any).__tw.state.loaded);
      await page.waitForTimeout(300);
      const bubble = page.locator('#sandbox-wrap').locator('xpath=..');
      const png = PNG.sync.read(await bubble.screenshot());
      const at = (x: number, y: number) => { const i = (y * png.width + x) * 4; return [png.data[i], png.data[i + 1], png.data[i + 2]]; };
      const s = Math.round(png.width / ((await bubble.boundingBox())!.width));
      const want = theme === 'dark' ? [0x21, 0x21, 0x21] : [0xff, 0xff, 0xff];
      // in the body's margin: the bubble's colour shows through the renderer, never a canvas of the renderer's own
      for (const [x, y] of [[6, 6], [png.width / s - 7, png.height / s - 4], [png.width / s / 2, 3]]) expect(at(Math.round(x * s), Math.round(y * s)), `${theme} ${x},${y}`).toEqual(want);
    }
  });

  test(`${renderer}: text is not selectable by default; user-select: text enables it, children included`, async ({ page }) => {
    const root = await load(page, `<p id="off">This text cannot be selected.</p><p id="on" style="user-select: text; -webkit-user-select: text">This text <b id="kid">can</b> be selected.</p>`, { shadow });
    // The renderer frame is sandboxed (an opaque origin): read its selection through Playwright, not from the page.
    const frame = shadow ? null : await (await page.locator('#sandbox-wrap iframe').elementHandle())!.contentFrame();
    const selected = () => frame ? frame.evaluate(() => getSelection()!.toString()) : page.evaluate(() => {
      const host = document.querySelector('#sandbox-wrap .widget-shadow')!;
      const r = (host.shadowRoot as any).getSelection?.() ?? document.getSelection();
      return r ? r.toString() : '';
    });
    const drag = async (sel: string) => {
      const b = (await root.locator(sel).boundingBox())!;
      await page.mouse.move(b.x + 2, b.y + b.height / 2);
      await page.mouse.down();
      await page.mouse.move(b.x + b.width - 2, b.y + b.height / 2, { steps: 8 });
      await page.mouse.up();
    };
    // Blink inherits user-select; the spec's `auto` would follow the parent to the same used value
    expect(await css(root.locator('#off'), 'user-select')).toMatch(/^(none|auto)$/);
    expect(await css(root.locator('#on'), 'user-select')).toBe('text');
    expect(await css(root.locator('#kid'), 'user-select')).toMatch(/^(text|auto)$/);
    await drag('#off');
    expect(await selected()).toBe('');
    await drag('#on');
    expect(await selected()).toContain('can be selected');
  });

  test(`${renderer}: each primary pointerdown reports which drag axes and holds the widget takes`, async ({ page }) => {
    const root = await load(page, `<style>div { height: 30px; margin: 4px; background: #ddd; }</style>
      <div id="none" style="touch-action: none">none</div><div id="pany" style="touch-action: pan-y">pan-y</div>
      <div id="panx" style="touch-action: pan-x">pan-x</div><div id="auto">auto</div>
      <div id="pd">prevented</div><script>document.getElementById('pd').addEventListener('pointerdown', function (e) { e.preventDefault(); });</script>`, { shadow });
    for (const id of ['none', 'pany', 'panx', 'auto', 'pd', 'pd']) await root.locator('#' + id).click();
    await expect.poll(async () => (await tw(page)).gestures.length).toBe(6);
    const gestures = (await tw(page)).gestures as { x: boolean; y: boolean; hold: boolean }[];
    // The first press on #pd teaches the renderer that the widget prevents it; the second one claims both axes (never holds).
    const c = (x: boolean, y: boolean, hold = false) => ({ x, y, hold });
    expect(gestures).toEqual([c(true, true, true), c(true, false), c(false, true), c(false, false), c(false, false), c(true, true)]);
  });

  test(`${renderer}: a tap opens one link and plays haptics; the actions reach the client`, async ({ page }) => {
    const root = await load(page, `<button id="b">go</button><a id="t" href="https://t.me/durov">t</a><script>
      document.getElementById('b').addEventListener('click', function () {
        TelegramWidget.HapticFeedback.impactOccurred('light');
        TelegramWidget.openLink('https://Example.com/é');
        TelegramWidget.openLink('https://example.com/second');
      });</script>`, { shadow });
    await root.locator('#b').click();
    await root.locator('#t').click();
    await expect.poll(async () => (await tw(page)).actions.length).toBe(3);
    expect((await tw(page)).actions).toEqual([
      { kind: 'haptic', haptic: 'impact', value: 'light' },
      { kind: 'openLink', url: 'https://example.com/%C3%A9' },
      { kind: 'openTelegramLink', url: 'https://t.me/durov' },
    ]);
    expect((await tw(page)).diags['activation-blocked']?.count).toBe(1);
  });
}

test('Web Storage: the client keeps what the widget commits and preloads it after a restart and a reload', async ({ page }) => {
  const widget = `<button id="b">+1</button><p id="n"></p><script>
    var n = Number(localStorage.getItem('n') || 0), s = Number(sessionStorage.getItem('s') || 0);
    function show() { document.getElementById('n').textContent = n + '/' + s; }
    document.getElementById('b').addEventListener('click', function () { localStorage.setItem('n', String(++n)); sessionStorage.setItem('s', String(++s)); show(); });
    show();</script>`;
  let root = await load(page, widget, { name: 'sdk/storage.html' });
  await page.evaluate(() => (window as any).__tw.clearStorage());
  root = await load(page, widget, { name: 'sdk/storage.html' });
  await expect(root.locator('#n')).toHaveText('0/0');
  await root.locator('#b').click();
  await root.locator('#b').click();
  await expect(root.locator('#n')).toHaveText('2/2');
  await page.evaluate(() => (window as any).__tw.restart());
  await page.waitForFunction(() => (window as any).__tw.state.loaded);
  root = page.frameLocator('#sandbox-wrap iframe').locator(':root');
  await expect(root.locator('#n')).toHaveText('2/2');
  expect(await page.evaluate(() => (window as any).__tw.storage())).toEqual({ local: { n: '2' }, session: { s: '2' } });
  // A reload is an app restart: `local` survives, `session` does not.
  root = await load(page, widget, { name: 'sdk/storage.html' });
  await expect(root.locator('#n')).toHaveText('2/0');
});

test('the client locale is navigator.language and the Intl default', async ({ page }) => {
  const root = await load(page, `<p id="l"></p><script>document.getElementById('l').textContent = navigator.language + ' ' + (1234.5).toLocaleString();</script>`, { query: '?locale=ru' });
  await expect(root.locator('#l')).toHaveText('ru 1 234,5');
});
