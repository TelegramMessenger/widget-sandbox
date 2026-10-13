// User activation (docs/decisions.md): privileged bridge calls need a real
// gesture. Playwright's clicks and key presses are trusted input
// (isTrusted = true); scripted clicks, timers and the frame's own
// dispatchEvent are not.

import { expect, test, type Page } from '@playwright/test';

test.skip(process.env.VITE_TW_POLICY === 'compat', 'the gesture gate belongs to the strict policy');

const WIDGET = `<button id="b">send</button><input id="i">
<script>
TelegramWidget.ready();
setTimeout(function () { TelegramWidget.sendMessage('timer'); }, 10);
document.getElementById('b').addEventListener('click', function () {
  TelegramWidget.sendMessage('clicked');
  TelegramWidget.sendMessage('second in the same gesture');
  setTimeout(function () { TelegramWidget.openLink('https://example.com/later'); }, 0);
});
document.getElementById('i').addEventListener('keydown', function (e) { if (e.key === 'Enter') TelegramWidget.sendMessage('enter'); });
setTimeout(function () { document.getElementById('b').click(); }, 50);
</script>`;

async function load(page: Page) {
  await page.goto('/');
  await page.evaluate((html) => (window as any).__tw.load(html, null), WIDGET);
  await page.waitForFunction(() => (window as any).__tw.state.loaded);
  return page.frameLocator('#sandbox-wrap iframe');
}
const calls = (page: Page) => page.evaluate(() => (window as any).__tw.state.bridge.map((b: any) => `${b.method}:${JSON.stringify(b.args)}`) as string[]);
const blocked = (page: Page) => page.evaluate(() => (window as any).__tw.state.diags['activation-blocked']?.count ?? 0);

test('timers and scripted clicks reach no privileged bridge method', async ({ page }) => {
  await load(page);
  await page.waitForTimeout(500);
  expect(await calls(page)).toEqual(['ready:[]']);
  expect(await blocked(page)).toBeGreaterThanOrEqual(3);
});

test('one real click unlocks exactly one call, made in its own handler', async ({ page }) => {
  const f = await load(page);
  await page.waitForTimeout(300);
  await f.locator('#b').click();
  await expect.poll(() => calls(page)).toContain('sendMessage:["clicked"]');
  await page.waitForTimeout(300);
  expect(await calls(page)).toEqual(['ready:[]', 'sendMessage:["clicked"]']);
  await f.locator('#b').click();
  await expect.poll(async () => (await calls(page)).length).toBe(3);
});

test('a real key press is a gesture too', async ({ page }) => {
  const f = await load(page);
  await f.locator('#i').click();
  await f.locator('#i').press('Enter');
  await expect.poll(() => calls(page)).toContain('sendMessage:["enter"]');
});

test('a hidden widget cannot use a gesture', async ({ page }) => {
  const f = await load(page);
  await page.evaluate(() => (window as any).__tw.setVisible(false));
  await f.locator('#b').click({ force: true });
  await page.waitForTimeout(500);
  expect(await calls(page)).toEqual(['ready:[]']);
});
