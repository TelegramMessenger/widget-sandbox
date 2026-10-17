// SDK v1 on the web host (docs/sdk-v1.md): the theme variables on the
// renderer root, gesture claims, Web Storage across a restart, the client
// locale and the actions a tap produces, on the iframe and the Shadow DOM
// renderer. The widget-facing API itself is unit-tested through QuickJS
// (packages/core/tests/unit/sdk.test.ts).

import { expect, test, type Locator, type Page } from '@playwright/test';

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

  test(`${renderer}: each primary pointerdown reports which drag axes the widget takes`, async ({ page }) => {
    const root = await load(page, `<style>div { height: 30px; margin: 4px; background: #ddd; }</style>
      <div id="none" style="touch-action: none">none</div><div id="pany" style="touch-action: pan-y">pan-y</div>
      <div id="panx" style="touch-action: pan-x">pan-x</div><div id="auto">auto</div>
      <div id="pd">prevented</div><script>document.getElementById('pd').addEventListener('pointerdown', function (e) { e.preventDefault(); });</script>`, { shadow });
    for (const id of ['none', 'pany', 'panx', 'auto', 'pd', 'pd']) await root.locator('#' + id).click();
    await expect.poll(async () => (await tw(page)).gestures.length).toBe(6);
    const gestures = (await tw(page)).gestures as { x: boolean; y: boolean }[];
    // The first press on #pd teaches the renderer that the widget prevents it; the second one claims both axes.
    expect(gestures).toEqual([{ x: true, y: true }, { x: true, y: false }, { x: false, y: true }, { x: false, y: false }, { x: false, y: false }, { x: true, y: true }]);
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
