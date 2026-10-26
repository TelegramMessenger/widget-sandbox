import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { sanitizeStylesheet, sanitizeStyleProperty } from '../../src/host/css-sanitizer.ts';

describe('native scrolling capability', () => {
  const css = '#v{height:170px;overflow-y:auto;overflow-x:hidden}#h{overflow-x:scroll;overflow-y:hidden}';
  const diag = () => {};

  it('keeps scrolling clipped unless the native host opts in', () => {
    const output = sanitizeStylesheet(css, diag, { colorScheme: 'light' });
    assert.doesNotMatch(output, /overflow-[xy]:(auto|scroll)/);
    assert.match(output, /overflow-y:hidden/);
  });

  it('preserves both scrolling axes for an opted-in native host', () => {
    const output = sanitizeStylesheet(css, diag, { colorScheme: 'light', nativeScrolling: true });
    assert.match(output, /overflow-y:auto/);
    assert.match(output, /overflow-x:scroll/);
    assert.equal(sanitizeStyleProperty('overflow-y', 'auto', diag, { colorScheme: 'dark', nativeScrolling: true })?.value, 'auto');
  });

  it('does not reuse opted-in CSS for a host with the default policy', () => {
    const before = sanitizeStylesheet(css, diag, { colorScheme: 'light' });
    sanitizeStylesheet(css, diag, { colorScheme: 'light', nativeScrolling: true });
    assert.equal(sanitizeStylesheet(css, diag, { colorScheme: 'light' }), before);
  });
});
