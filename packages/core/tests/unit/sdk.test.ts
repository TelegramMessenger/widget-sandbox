// SDK v1 (docs/sdk-v1.md §2) end to end through the real sandbox bundle in
// QuickJS: the widget-facing API, and the glue's checks as the Node harness
// mirrors them from WidgetHost (activation, the haptic limit, storage commits).

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';
import { THEME_KEYS } from '../../src/shared/sdk-policy.ts';

const idOf = async (html: string, tag: string) => ((await runWidget(html)).renderOps.find((o: any) => o.op === 'el' && o.tag === tag) as any).id as number;
const diags = (r: { diags: { kind: string; detail: string }[] }, kind: string) => r.diags.filter((d) => d.kind === kind);

describe('SDK v1: TelegramWidget members', () => {
  it('has the v1 members and no sendMessage', async () => {
    const r = await runWidget('<p>x</p>', {
      probes: [
        'TelegramWidget.profile', 'TelegramWidget.isProfileAtLeast(1)', 'TelegramWidget.isProfileAtLeast(2)', 'TelegramWidget.isProfileAtLeast("1")',
        'TelegramWidget.colorScheme', 'TelegramWidget.theme', 'TelegramWidget.isActive', 'typeof TelegramWidget.sendMessage', 'typeof TelegramWidget.on',
        'Object.isFrozen(TelegramWidget.themeParams)', 'Object.keys(TelegramWidget.themeParams).sort().join()',
        'TelegramWidget.HapticFeedback.impactOccurred("light") === TelegramWidget.HapticFeedback',
        'TelegramWidget.HapticFeedback.notificationOccurred("success") === TelegramWidget.HapticFeedback',
        'TelegramWidget.HapticFeedback.selectionChanged() === TelegramWidget.HapticFeedback',
      ],
    });
    assert.deepEqual(r.probes, ['1', 'true', 'false', 'false', 'light', 'light', 'true', 'undefined', 'undefined', 'true', [...THEME_KEYS].sort().join(), 'true', 'true', 'true']);
  });

  it('themeParams come from the client, checked, the rest from the scheme defaults; a change fires themeChanged and onTheme once', async () => {
    const widget = `<script>
      var log = [], first = TelegramWidget.themeParams;
      TelegramWidget.onEvent('themeChanged', function () { log.push('changed:' + TelegramWidget.colorScheme + ':' + TelegramWidget.themeParams.bg_color + ':' + (this === TelegramWidget)); });
      TelegramWidget.onTheme(function (t) { log.push('onTheme:' + t); });
    </script>`;
    const r = await runWidget(widget, {
      themeParams: { bg_color: '#ABCDEF', text_color: 'red', made_up: '#123456' },
      script: [
        { probe: 'TelegramWidget.themeParams.bg_color + "," + TelegramWidget.themeParams.text_color + "," + ("made_up" in TelegramWidget.themeParams)' },
        { theme: 'light', themeParams: { bg_color: '#abcdef' } },
        { theme: 'dark', themeParams: { bg_color: '#101010' } },
        { probe: 'log.join("|")' },
        { probe: 'first === TelegramWidget.themeParams' },
        { probe: 'TelegramWidget.themeParams.text_color' },
      ],
    });
    assert.deepEqual(r.probes, ['#abcdef,#000000,false', 'onTheme:light|changed:dark:#101010:true|onTheme:dark', 'false', '#ffffff']);
  });

  it('onEvent: known types only, functions only, no duplicates, 16 per type; offEvent removes', async () => {
    const r = await runWidget(`<script>
      var n = 0, f = function () { n++; };
      TelegramWidget.onEvent('activated', f); TelegramWidget.onEvent('activated', f);
      for (var i = 0; i < 20; i++) TelegramWidget.onEvent('deactivated', function () { n += 100; });
      TelegramWidget.onEvent('linkOpened', f); TelegramWidget.onEvent('activated', 'f');
    </script>`, {
      visible: false,
      script: [{ visible: true }, { probe: 'n' }, { visible: false }, { probe: 'n' }, { probe: '(TelegramWidget.offEvent("activated", f), n)' }, { visible: true }, { probe: 'n + ":" + TelegramWidget.isActive' }],
    });
    assert.deepEqual(r.probes, ['1', '1601', '1601', '1601:true']);
  });
});

describe('SDK v1: links', () => {
  const widget = `<button>go</button><a id="ext" href="https://example.com/a">a</a><a id="tg" href="https://t.me/durov">t</a><a id="http" href="http://example.com/">h</a><a id="js" href="javascript:void 0">j</a><script>
    var errors = [];
    function tryOpen(m, u) { try { TelegramWidget[m](u); } catch (e) { errors.push(m + ':' + e.name); } }
    tryOpen('openLink', 'http://example.com/'); tryOpen('openLink', 'https://t.me/x'); tryOpen('openLink', 'https://example.com/' + 'a'.repeat(300));
    tryOpen('openTelegramLink', 'tg://resolve?domain=x'); tryOpen('openTelegramLink', 'https://example.com/'); tryOpen('openLink', 42);
    document.querySelector('button').addEventListener('click', function () { TelegramWidget.openLink('HTTPS://Example.com/поиск'); TelegramWidget.openLink('https://example.com/second'); });
  </script>`;

  it('invalid URLs throw TypeError in the widget and never reach the host', async () => {
    const r = await runWidget(widget, { probes: ['errors.join()'] });
    assert.equal(r.probes[0], 'openLink:TypeError,openLink:TypeError,openLink:TypeError,openTelegramLink:TypeError,openTelegramLink:TypeError,openLink:TypeError');
    assert.deepEqual(r.actions, []);
  });

  it('a tap opens one link, normalised; the second call of the same tap is blocked', async () => {
    const button = await idOf(widget, 'button');
    const r = await runWidget(widget, { events: [{ type: 'click', targetId: button, gesture: 1 }] });
    assert.deepEqual(r.actions, [{ kind: 'openLink', url: 'https://example.com/%D0%BF%D0%BE%D0%B8%D1%81%D0%BA' }]);
    assert.equal(diags(r, 'activation-blocked').length, 1);
  });

  it('<a href> taps go to openLink or openTelegramLink; http: and javascript: are ignored', async () => {
    const first = await runWidget(widget);
    const anchors = first.renderOps.filter((o: any) => o.op === 'el' && o.tag === 'a').map((o: any) => o.id);
    const r = await runWidget(widget, { events: anchors.map((id: number, i: number) => ({ type: 'click', targetId: id, gesture: i + 1 })) });
    assert.deepEqual(r.actions, [{ kind: 'openLink', url: 'https://example.com/a' }, { kind: 'openTelegramLink', url: 'https://t.me/durov' }]);
    assert.equal(diags(r, 'navigation-blocked').length, 2);
  });

  it('window.open takes the same path and returns null', async () => {
    const html = '<button>o</button><script>var w; document.querySelector("button").onclick = function () { w = window.open("https://telegram.me/x"); };</script>';
    const r = await runWidget(html, { events: [{ type: 'click', targetId: await idOf(html, 'button'), gesture: 1 }], probes: ['w'] });
    assert.deepEqual(r.actions, [{ kind: 'openTelegramLink', url: 'https://telegram.me/x' }]);
    assert.equal(r.probes[0], 'null');
  });
});

describe('SDK v1: haptics', () => {
  it('need no tap; a burst of 5, the rest dropped with one haptic-limit; unknown styles throw', async () => {
    let t = 0;
    const r = await runWidget(`<script>
      for (var i = 0; i < 7; i++) TelegramWidget.HapticFeedback.impactOccurred('medium');
      var bad = []; ['loud', undefined].forEach(function (s) { try { TelegramWidget.HapticFeedback.impactOccurred(s); } catch (e) { bad.push(e.name); } });
      try { TelegramWidget.HapticFeedback.notificationOccurred('light'); } catch (e) { bad.push(e.name); }
    </script>`, { now: () => t, probes: ['bad.join()'] });
    assert.equal(r.actions.length, 5);
    assert.deepEqual(r.actions[0], { kind: 'haptic', haptic: 'impact', value: 'medium' });
    assert.equal(diags(r, 'haptic-limit').length, 1);
    assert.equal(r.probes[0], 'TypeError,TypeError,TypeError');
  });

  it('a hidden widget plays nothing', async () => {
    const r = await runWidget('<script>TelegramWidget.HapticFeedback.selectionChanged();</script>', { visible: false });
    assert.deepEqual(r.actions, []);
    assert.equal(diags(r, 'haptic-limit').length, 1);
  });
});

describe('SDK v1: storage', () => {
  it('the preload is there from the first line; each task commits the areas it changed, in full', async () => {
    const html = `<button>b</button><script>
      var seen = localStorage.getItem('count') + '/' + sessionStorage.getItem('s') + '/' + localStorage.length;
      localStorage.setItem('count', String(Number(localStorage.getItem('count')) + 1));
      localStorage.extra = 7;
      document.querySelector('button').onclick = function () { sessionStorage.setItem('s', 'tapped'); localStorage.setItem('count', localStorage.count); };
    </script>`;
    const r = await runWidget(html, { storage: { local: { count: '41' }, session: { s: 'kept' } }, events: [{ type: 'click', targetId: await idOf(html, 'button') }], probes: ['seen'] });
    assert.equal(r.probes[0], '41/kept/1');
    assert.deepEqual(r.storageCommits, [{ local: { count: '42', extra: '7' } }, { session: { s: 'tapped' } }], 'one commit per task; an unchanged value is no change');
  });

  it('16 KB of UTF-8 per area: setItem past it throws QuotaExceededError and stores nothing', async () => {
    const r = await runWidget(`<script>
      var out = [];
      localStorage.setItem('k', 'x'.repeat(16383));
      try { localStorage.setItem('k2', 'y'); } catch (e) { out.push(e.name, e.code, e instanceof DOMException, localStorage.getItem('k2')); }
      try { localStorage.big = 'ж'.repeat(9000); } catch (e) { out.push(e.name); }
      sessionStorage.setItem('k', 'x'.repeat(16000));
      localStorage.removeItem('k'); localStorage.setItem('w', 'ж'.repeat(8191));
    </script>`, { probes: ['out.join()', 'localStorage.length', 'sessionStorage.getItem("k").length'] });
    assert.deepEqual(r.probes, ['QuotaExceededError,22,true,,QuotaExceededError', '1', '16000']);
    assert.equal(r.storageCommits.length, 1);
    assert.deepEqual(Object.keys(r.storageCommits[0].local!), ['w']);
    assert.equal(r.storageCommits[0].session!.k.length, 16000);
  });

  it('a preload over the quota is dropped whole: storage-rejected, the widget starts empty', async () => {
    const r = await runWidget('<p>x</p>', { storage: { local: { a: 'x'.repeat(20000) }, session: { b: '1' } }, probes: ['localStorage.length + sessionStorage.length'] });
    assert.equal(diags(r, 'storage-rejected').length, 1);
    assert.equal(r.probes[0], '0');
  });

  it('clear() on an empty area and reads commit nothing', async () => {
    const r = await runWidget('<script>localStorage.clear(); localStorage.getItem("a"); sessionStorage.removeItem("x");</script>');
    assert.deepEqual(r.storageCommits, []);
  });
});

describe('SDK v1: locale and inert APIs', () => {
  it('navigator.language and the Intl default follow the client locale', async () => {
    const r = await runWidget('<p>x</p>', { locale: 'ru', probes: ['navigator.language', 'navigator.languages.join()', 'new Intl.NumberFormat().resolvedOptions().locale', '(1234.5).toLocaleString()', 'new Intl.NumberFormat("en-US").format(1234.5)'] });
    assert.deepEqual(r.probes, ['ru', 'ru', 'ru', '1 234,5', '1,234.5']);
  });

  it('a malformed locale is en-US', async () => {
    const r = await runWidget('<p>x</p>', { locale: 'ru_RU', probes: ['navigator.language'] });
    assert.equal(r.probes[0], 'en-US');
  });

  it('alert, confirm and prompt show nothing and send nothing; clipboard and share reject with NotAllowedError', async () => {
    const r = await runWidget(`<button>c</button><script>
      var out = [alert('a'), confirm('c'), prompt('p')];
      document.querySelector('button').onclick = function () {
        navigator.clipboard.writeText('x').catch(function (e) { out.push(e.name); });
        navigator.share({ text: 'x' }).catch(function (e) { out.push(e.name); });
      };
    </script>`, { events: [{ type: 'click', targetId: 5, gesture: 1 }], probes: ['JSON.stringify(out)'] });
    assert.deepEqual(r.bridge, []);
    assert.equal(r.probes[0], JSON.stringify([null, false, null, 'NotAllowedError', 'NotAllowedError']));
  });
});
