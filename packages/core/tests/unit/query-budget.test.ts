// The layout-query budget through the Node harness (the twin of QueryBudgetTests.swift,
// QueryBudgetTest.kt and the desktop layout_query_loop tests): a forced-layout loop whose
// queries cost the host time is interrupted and reported with the layout-query reason, an
// ordinary layout-heavy widget is not. The harness answers queries in-process, so `answer`
// stands in for a slow renderer.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';
import { queryBudgetMs, QUERY_BUDGET_MS } from '../../src/host/cpu-budget.ts';

const busy = (ms: number) => { const t = performance.now(); while (performance.now() - t < ms) { /* a slow renderer */ } };
const html = (script: string) => `<div id="a"></div><script>${script}</script>`;

describe('layout-query budget', () => {
  it('is 100 ms, whatever the slice (boot included)', () => {
    assert.equal(queryBudgetMs(500), QUERY_BUDGET_MS);
    assert.equal(queryBudgetMs(10), QUERY_BUDGET_MS);
    assert.equal(QUERY_BUDGET_MS, 100);
  });

  it('kills a forced-layout loop with the layout-query reason', async () => {
    const started = performance.now();
    const r = await runWidget(html("const a = document.getElementById('a'); let i = 0; for (;;) { a.style.width = (i++ % 300) + 'px'; a.offsetHeight; }"), {
      cpuLimitMs: 200,
      answer: () => { busy(5); return undefined; }, // the canned answer, after a slow renderer's delay
    });
    const reason = r.logs.find((l) => l.level === 'error' && /Layout query limit exceeded/.test(l.text));
    assert.ok(reason, JSON.stringify(r.logs.slice(0, 3)));
    assert.match(reason!.text, /more than 100 ms in one boot task/);
    assert.ok(r.diags.some((d) => d.kind === 'query-limit'));
    assert.ok(r.queries.length < 120, `${r.queries.length} queries`);
    assert.ok(performance.now() - started < 5000, 'the harness returned promptly');
  });

  it('leaves an ordinary layout-heavy widget alive', async () => {
    const r = await runWidget(html("const a = document.getElementById('a'); for (let i = 0; i < 10; i++) { a.style.width = i + 'px'; a.offsetHeight; }"), {
      cpuLimitMs: 200,
      answer: () => { busy(2); return undefined; },
    });
    assert.equal(r.logs.filter((l) => l.level === 'error').length, 0, JSON.stringify(r.logs));
    assert.ok(r.queries.length > 0);
  });
});
