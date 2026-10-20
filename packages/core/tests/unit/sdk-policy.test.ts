// src/shared/sdk-policy.ts against the fixture shared with SdkPolicy.swift,
// SdkPolicy.kt and sdk_policy.cpp (tests/fixtures/sdk-policy.json), plus the
// TS-only parts: theme defaults and the renderer's variables.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { describe, it } from 'node:test';
import {
  HapticGate, THEME_KEYS, THEME_VARIABLES, checkBridgeCall, checkHaptic, checkLink, checkLocale, checkStorageCommit, checkThemeParams,
  DEFAULT_TEXT_TOKENS, completeThemeParams, themeVariables, utf8Length, type LinkMethod,
} from '../../src/shared/sdk-policy.ts';
import { PROFILE_UA_STYLESHEET } from '../../src/shared/profile-tables.ts';

const fixture = JSON.parse(fs.readFileSync(path.join(path.dirname(new URL(import.meta.url).pathname), '..', 'fixtures', 'sdk-policy.json'), 'utf8'));

/** `{ "repeat": [s, n] }` stands for s repeated n times. */
const expand = (v: unknown): unknown => {
  if (Array.isArray(v)) return v.map(expand);
  if (v && typeof v === 'object') {
    const o = v as Record<string, unknown>;
    if (Object.keys(o).length === 1 && Array.isArray(o.repeat)) return String(o.repeat[0]).repeat(Number(o.repeat[1]));
    return Object.fromEntries(Object.entries(o).map(([k, x]) => [k, expand(x)]));
  }
  return v;
};

describe('sdk-policy fixture', () => {
  it('links', () => {
    for (const c of fixture.links) assert.equal(checkLink(c.method as LinkMethod, c.url), c.expect, `${c.method} ${String(c.url).slice(0, 80)}`);
  });
  it('haptics', () => {
    for (const c of fixture.haptics) assert.deepEqual(checkHaptic(c.args), c.expect, JSON.stringify(c.args));
  });
  it('bridge calls', () => {
    for (const c of fixture.bridge) assert.deepEqual(checkBridgeCall(c.method, c.args), c.expect, `${c.method} ${JSON.stringify(c.args)}`);
  });
  it('haptic gate', () => {
    for (const c of fixture.hapticGate) {
      const gate = new HapticGate();
      c.steps.forEach((s: { at: number; visible: boolean; expect: string }, i: number) => assert.equal(gate.decide(s.at, s.visible), s.expect, `${c.name}, step ${i}`));
    }
  });
  it('theme params', () => {
    for (const c of fixture.themeParams) assert.deepEqual(checkThemeParams(c.input), c.expect, JSON.stringify(c.input));
  });
  it('locales', () => {
    for (const c of fixture.locales) assert.equal(checkLocale(c.input), c.expect, String(c.input));
  });
  it('storage', () => {
    for (const c of fixture.storage) assert.deepEqual(checkStorageCommit(expand(c.input)), expand(c.expect), c.note ?? JSON.stringify(c.input));
  });
  it('utf8', () => {
    for (const c of fixture.utf8) assert.equal(utf8Length(c.s), c.bytes, c.s);
  });
});

describe('sdk-policy (TS only)', () => {
  it('a lone surrogate is no URL and counts 3 bytes', () => {
    assert.equal(checkLink('openLink', 'https://example.com/\ud800'), null);
    assert.equal(utf8Length('\udc00'), 3);
  });
  it('fills the theme keys the client left out from the scheme defaults', () => {
    const p = completeThemeParams({ bg_color: '#ABCDEF', text_color: 'red' }, 'dark');
    assert.deepEqual(Object.keys(p).sort(), [...THEME_KEYS].sort());
    assert.equal(p.bg_color, '#abcdef');
    assert.equal(p.text_color, '#ffffff');
  });
  it('builds the renderer variables from the fixed list, dropping what breaks the grammar', () => {
    const vars = themeVariables({
      colorScheme: 'dark',
      params: { link_color: '#112233' },
      button: { bg: '#3390EC', text: 'white', radius: '10px', font: '600 15px/1.2 system-ui, sans-serif', padding: '8px 14px', height: '36px', pressedOpacity: 0.7, disabledOpacity: '2' },
    });
    assert.equal(vars['--tg-color-scheme'], 'dark');
    assert.equal(vars['--tg-theme-link-color'], '#112233');
    assert.equal(vars['--tg-theme-bg-color'], '#212121');
    assert.equal(vars['--tg-button-bg'], '#3390ec');
    assert.equal(vars['--tg-button-text'], undefined);
    assert.equal(vars['--tg-button-radius'], '10px');
    assert.equal(vars['--tg-button-font'], '600 15px/1.2 system-ui, sans-serif');
    assert.equal(vars['--tg-button-padding'], '8px 14px');
    assert.equal(vars['--tg-button-height'], '36px');
    assert.equal(vars['--tg-button-pressed-opacity'], '0.7');
    assert.equal(vars['--tg-button-disabled-opacity'], undefined);
    for (const name of Object.keys(vars)) assert.ok(THEME_VARIABLES.includes(name), name);
  });
  it('a button font must name generic families only', () => {
    assert.equal(themeVariables({ button: { font: '15px Helvetica' } })['--tg-button-font'], undefined);
    assert.equal(themeVariables({ button: { font: '15px system-ui; color: red' } })['--tg-button-font'], undefined);
    assert.equal(themeVariables({ button: { font: 'italic bold 15px ui-rounded' } })['--tg-button-font'], 'italic bold 15px ui-rounded');
  });
  it('a theme that is not an object is the light defaults', () => {
    assert.equal(themeVariables('dark')['--tg-color-scheme'], 'light');
    assert.equal(themeVariables(null)['--tg-theme-bg-color'], '#ffffff');
  });
  it('the text tokens are always set: the client\'s where they match the grammar, the reference set otherwise', () => {
    const ref = themeVariables({ colorScheme: 'light' });
    assert.equal(ref['--tg-text-font-size'], DEFAULT_TEXT_TOKENS.fontSize);
    assert.equal(ref['--tg-text-line-height'], DEFAULT_TEXT_TOKENS.lineHeight);
    assert.equal(ref['--tg-text-padding'], DEFAULT_TEXT_TOKENS.padding);
    assert.deepEqual(DEFAULT_TEXT_TOKENS, { fontSize: '15px', lineHeight: '1.35', padding: '8px 12px' });
    const t = (text: unknown) => themeVariables({ colorScheme: 'light', text });
    assert.equal(t({ fontSize: 17 })['--tg-text-font-size'], '17px');
    assert.equal(t({ fontSize: 17.5 })['--tg-text-font-size'], '17.5px');
    assert.equal(t({ fontSize: 15.0000001 })['--tg-text-font-size'], '15px');
    assert.equal(t({ fontSize: '16px' })['--tg-text-font-size'], '16px');
    for (const bad of [7, 65, '16', '1em', '16 px', 'calc(16px)', NaN, Infinity, -15, null, '9999px']) assert.equal(t({ fontSize: bad })['--tg-text-font-size'], '15px', String(bad));
    assert.equal(t({ lineHeight: 1.2 })['--tg-text-line-height'], '1.2');
    assert.equal(t({ lineHeight: '1.5' })['--tg-text-line-height'], '1.5');
    for (const bad of [0.9, 3.5, '20px', '1.2em', 'normal']) assert.equal(t({ lineHeight: bad })['--tg-text-line-height'], '1.35', String(bad));
    assert.equal(t({ padding: '10px 14px' })['--tg-text-padding'], '10px 14px');
    assert.equal(t({ padding: '0' })['--tg-text-padding'], '0');
    for (const bad of ['10px;color:red', '1px 2px 3px 4px 5px', 'auto', 12]) assert.equal(t({ padding: bad })['--tg-text-padding'], '8px 12px', String(bad));
    for (const name of ['--tg-text-font-size', '--tg-text-line-height', '--tg-text-padding']) assert.ok(THEME_VARIABLES.includes(name), name);
  });
  it('the UA layer is one @layer declared first, every selector of it in :where(), the text and root defaults on the tokens', () => {
    const ua = PROFILE_UA_STYLESHEET;
    assert.ok(ua.startsWith('@layer tw-ua{'));
    const layer = ua.slice('@layer tw-ua{'.length, ua.lastIndexOf('}*{'));
    for (const m of layer.matchAll(/(?:^|})([^{}]+)\{/g)) assert.ok(m[1].startsWith(':where('), m[1]);
    for (const v of ['--tg-theme-bg-color', '--tg-theme-text-color', '--tg-theme-link-color', '--tg-text-font-size', '--tg-text-line-height', '--tg-text-padding']) assert.ok(layer.includes(`var(${v}`), v);
    // rem would resolve against the chat page's root on the shadow-DOM renderer
    assert.ok(!/[0-9]rem\b/.test(layer));
  });
});
