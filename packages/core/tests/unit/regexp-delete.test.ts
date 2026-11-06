// Regression for the RegExp deletion use-after-free (upstream quickjs-ng
// 3d5cb148a7f8, backported into the fork at fix/regexp-delete-lastindex).
// JS_RegExpDelete snapshotted the compiled bytecode before converting
// lastIndex, and that conversion can run JavaScript. corpus/attacks/
// regexp-delete-recompile.html recompiles the same RegExp during the
// conversion and pushes the old bytecode out of the fork's pattern cache; the
// deletion loop then read and ran the freed allocation. On the unfixed engine
// the shipped wasm aborts its WebAssembly runtime on this widget; the fixed
// engine recompiles to /b/ first and returns deterministic, correct results.
// Running it through the Node/wasm harness checks packages/quickjs-wasm/dist,
// not only the native builds.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';
import { runWidget } from '../../scripts/sandbox-node.ts';

const attack = fs.readFileSync(
  path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..', '..', 'corpus', 'attacks', 'regexp-delete-recompile.html'),
  'utf8',
);

describe('RegExp deletion bytecode lifetime', () => {
  it('replace(re, "") after a recompile during lastIndex uses the new pattern and never reads freed bytecode', async () => {
    const r = await runWidget(attack, { timerRounds: 0, probes: ['window.rceResult'] });
    // Both halves (plain, and with the fork's pattern cache evicted): re became
    // /b/g during the conversion, so "aaa".replace stays "aaa", re.source is
    // "b", lastIndex is reset to 0, and "abab".replace is "aa".
    assert.equal(r.probes[0], 'aaa|b|0|aa aaa|b|0|aa');
    assert.deepEqual(r.logs.filter((l) => l.level === 'error').map((l) => l.text), []);
  });
});
