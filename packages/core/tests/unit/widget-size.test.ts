// The widget's size as the server counts it (src/shared/widget-size.ts, the harness's `size:` line).

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { MINI_HTML, sizeLine, sizeVerdict } from '../../src/shared/widget-size.ts';
import { widgetSize } from '../../scripts/sandbox-node.ts';

describe('widget size', () => {
  it('counts gzip at level 6 and UTF-8 bytes', () => {
    const s = widgetSize('<p>привет</p>');
    assert.equal(s.raw, 19);
    assert.ok(s.gzip > 20 && s.gzip < 60, String(s.gzip));
  });
  it('says nothing under 3800 B, warns up to 4096 B, errs past either limit', () => {
    assert.deepEqual(sizeVerdict({ gzip: 3800, raw: 9000 }), { level: 'ok', message: null });
    assert.equal(sizeVerdict({ gzip: 3801, raw: 9000 }).level, 'warning');
    assert.equal(sizeVerdict({ gzip: MINI_HTML.gzip, raw: 9000 }).level, 'warning');
    const big = sizeVerdict({ gzip: 4097, raw: 40000 });
    assert.equal(big.level, 'error');
    assert.match(big.message!, /INPUT_MEDIA_WIDGET_TOO_LARGE.*over the 4096 B limit by 1 B; 40000 B of HTML is over the 32768 B limit by 7232 B/);
    assert.equal(sizeLine({ gzip: 2134, raw: 10035 }), 'size: 2134 B gzip / 9.8 KB raw (limit 4096 / 32768)');
  });
});
