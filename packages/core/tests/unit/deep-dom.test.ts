// A deep but legal document: the shim's mirror (capture.ts: creation, the full walk, removal), the parser's tree
// building (dom.ts) and the script scan (index.ts) walk it with loops, not recursions: each level of a recursion was a
// QuickJS call on the glue's native stack, and a web worker's ran out near 1,000 levels ("sandbox glue error: Maximum
// call stack size exceeded"; Node's near 1,900). The runs hold QuickJS to 128 KB of stack, where the old recursions
// stopped near 350 levels, so a recursion per level fails here on any host. A widget nested past the validator's
// 1024-deep cap (v0.6.5) meets the nesting-depth limit, and past 2000 nodes the node limit, as any other widget.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';

const STACK = 128 * 1024;
/** Nested elements well above the ~350 a 128 KB stack held by recursion, and under the 1024 nesting-depth cap. */
const DEPTH = 1000;

/** Levels from the deepest element up to <html>. */
const DEPTH_PROBE = "(function(){var n=document.querySelector('#deepest'),d=0;for(;n&&n.nodeType===1;n=n.parentNode)d++;return String(d);})()";

const errorsOf = (r: Awaited<ReturnType<typeof runWidget>>) => r.logs.filter((l) => l.level === 'error').map((l) => l.text);

const built = (depth: number) => `<div id="r"></div><script>
  var p = document.getElementById('r');
  for (var i = 0; i < ${depth}; i++) { var d = document.createElement('div'); p.appendChild(d); p = d; }
  p.id = 'deepest';
  p.appendChild(document.createTextNode('deep'));
</script>`;

describe('deep documents: no recursion per level', () => {
  it(`${DEPTH} nested elements built by script boot, every one reaches the renderer`, async () => {
    const r = await runWidget(built(DEPTH), { maxStackBytes: STACK, probes: [DEPTH_PROBE] });
    assert.deepEqual(errorsOf(r), []);
    assert.equal(r.stopped, undefined);
    assert.ok(r.nodes <= 2000, String(r.nodes));
    // div#r, the nested divs, <body> and <html>
    assert.equal(r.probes[0], String(DEPTH + 3));
    assert.equal(r.renderOps.flat().filter((o: any) => o.op === 'el' && o.tag === 'div').length, DEPTH + 1);
  });

  it(`${DEPTH} nested elements in the markup (the parser's tree building) and through innerHTML`, async () => {
    const markup = `<body>${'<div>'.repeat(DEPTH - 1)}<div id="deepest">deep</div>${'</div>'.repeat(DEPTH - 1)}</body>`;
    const a = await runWidget(markup, { maxStackBytes: STACK, probes: [DEPTH_PROBE] });
    assert.deepEqual(errorsOf(a), []);
    assert.equal(a.stopped, undefined);
    assert.equal(a.probes[0], String(DEPTH + 2));
    const inner = `<div id="r"></div><script>
      document.getElementById('r').innerHTML = '${'<b>'.repeat(DEPTH - 1)}<b id="deepest">x</b>${'</b>'.repeat(DEPTH - 1)}';
    </script>`;
    const b = await runWidget(inner, { maxStackBytes: STACK, probes: [DEPTH_PROBE] });
    assert.deepEqual(errorsOf(b), []);
    assert.equal(b.stopped, undefined);
    assert.equal(b.probes[0], String(DEPTH + 3));
  });

  it('a deep subtree moved to another parent and then removed leaves the mirror and the validator consistent', async () => {
    const html = `<div id="r"></div><div id="s"></div><script>
      var r = document.getElementById('r'), p = r;
      for (var i = 0; i < ${DEPTH - 2}; i++) { var d = document.createElement('div'); p.appendChild(d); p = d; }
      setTimeout(function () {
        document.getElementById('s').appendChild(r.firstChild);
        setTimeout(function () { document.getElementById('s').textContent = 'gone'; }, 1);
      }, 1);
    </script>`;
    const r = await runWidget(html, { maxStackBytes: STACK, timerRounds: 3, probes: ['String(document.getElementsByTagName("*").length)', 'document.getElementById("s").textContent'] });
    assert.deepEqual(errorsOf(r), []);
    assert.equal(r.stopped, undefined);
    assert.deepEqual(r.probes, ['6', 'gone']);
    const ops = r.renderOps.flat() as any[];
    const top = ops.find((o) => o.op === 'el' && o.tag === 'div' && !o.attrs?.length)!.id;
    assert.ok(ops.some((o) => o.op === 'rm' && o.id === top), 'the moved subtree is removed in the renderer');
    // after the removal the widget is small again: a new deep subtree of the same size fits under the cap
    assert.ok(!r.diags.some((d) => d.kind.includes('limit')), JSON.stringify(r.diags));
  });

  it('1100 nested elements stop at the nesting-depth limit, not in the glue (the glue builds the whole chain first)', async () => {
    // Past the 1024-deep cap but under 2000 nodes: the glue builds all 1100 with loops (no stack overflow), the
    // validator then stops the widget at the depth limit. A linear chain meets this cap before the node cap; the node
    // cap is for a wide document (strict-limits.test.ts).
    const r = await runWidget(built(1100), { maxStackBytes: STACK });
    assert.match(r.stopped ?? '', /^Nesting depth limit exceeded/);
    assert.ok(!errorsOf(r).some((t) => /stack/i.test(t)), errorsOf(r).join('\n'));
  });
});
