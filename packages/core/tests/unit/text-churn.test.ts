import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { complexWords, Validator } from '../../src/host/validator.ts';

const BASE = [
  { op: 'el', id: 2, tag: 'html' }, { op: 'ins', parent: 1, id: 2, before: null },
  { op: 'el', id: 3, tag: 'body' }, { op: 'ins', parent: 2, id: 3, before: null },
];

function rig() {
  const diags: string[] = [];
  const v = new Validator((kind, detail) => diags.push(`${kind}: ${detail}`));
  let now = 1000;
  v.setClock(() => now);
  v.processOps(BASE);
  return { v, diags, advance: (s: number) => { now += s; } };
}

const cjk = (n: number, c = 0x4e2d) => String.fromCharCode(c).repeat(n);

describe('complex-script text churn', () => {
  it('counts words, not units', () => {
    assert.equal(complexWords('BTC $60000 ▲'), 1);
    assert.equal(complexWords('中文字 hello मेरा नाम'), 5);
    assert.equal(complexWords('あいうえおかきくけこ。さしす'), 1);
    assert.equal(complexWords(''), 0);
  });

  it('rate-limits complex-script changes and refills with time', () => {
    const r = rig();
    assert.equal(r.v.processOps([{ op: 'text', id: 20, data: cjk(3000) }, { op: 'ins', parent: 3, id: 20, before: null }]).length, 2);
    let accepted = 0;
    for (let i = 0; i < 4; i++) accepted += r.v.processOps([{ op: 'data', id: 20, data: cjk(2999) }, { op: 'data', id: 20, data: cjk(3000) }]).length;
    assert.equal(accepted, 1);
    assert.ok(r.diags.some((d) => d.startsWith('text-churn: ')));
    r.advance(6);
    assert.equal(r.v.processOps([{ op: 'data', id: 20, data: cjk(2999) }]).length, 1);
  });

  it('never limits tickers, typewriters or latin text', () => {
    const r = rig();
    r.v.processOps([{ op: 'text', id: 20, data: 'x' }, { op: 'ins', parent: 3, id: 20, before: null }]);
    const line = 'मेरा नाम है सैम';
    for (let n = 0; n < 5000; n++) {
      r.advance(0.01);
      r.v.processOps([{ op: 'data', id: 20, data: `BTC $${60000 + n} ▲ ETH ${line.slice(0, n % line.length)}` }]);
      r.v.processOps([{ op: 'data', id: 20, data: 'x'.repeat(n % 4000) }]);
    }
    assert.deepEqual(r.diags.filter((d) => d.startsWith('text-churn')), []);
  });

  it('rejects a fresh complex text node over the churn budget even when the document budget allows it', () => {
    const r = rig();
    r.v.processOps([{ op: 'text', id: 20, data: cjk(4000) }, { op: 'ins', parent: 3, id: 20, before: null }, { op: 'rm', id: 20 }]);
    r.v.processOps([{ op: 'text', id: 21, data: cjk(4000, 0x6587) }, { op: 'ins', parent: 3, id: 21, before: null }, { op: 'rm', id: 21 }]);
    const out = r.v.processOps([{ op: 'text', id: 22, data: cjk(4000, 0x5b57) }, { op: 'ins', parent: 3, id: 22, before: null }]);
    assert.ok(!JSON.stringify(out).includes(cjk(4000, 0x5b57)));
    assert.ok(r.diags.some((d) => d.startsWith('text-churn: ')));
    assert.ok(!r.diags.some((d) => d.startsWith('text-budget')));
  });
});
