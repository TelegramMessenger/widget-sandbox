import assert from 'node:assert/strict';
import fs from 'node:fs';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';

const corpus = new URL('../../../../corpus/showcase/3d/', import.meta.url);
const files = fs.readdirSync(corpus).filter(file => file.endsWith('.html')).sort();
const state = 'JSON.stringify({yaw,pitch,zoom,variant,drag})';

describe('projected 3D showcase widgets', () => {
  it('contains exactly five self-contained widgets', () => {
    assert.equal(files.length, 5);
  });

  for (const file of files) {
    const html = fs.readFileSync(new URL(file, corpus), 'utf8');

    it(`${file}: validates and draws finite geometry in both themes`, async () => {
      for (const theme of ['light', 'dark'] as const) {
        const result = await runWidget(html, { theme, policy: 'strict', canvasWords: true,
          probes: ['canvas.width', 'canvas.height'] });
        assert.deepEqual(result.diags, []);
        assert.deepEqual(result.logs.filter(log => log.level === 'error'), []);
        assert.ok(result.size.zlib <= 3800);
        assert.ok(result.size.raw <= 32767);
        assert.ok(Number(result.probes[0]) > 0);
        assert.ok(Number(result.probes[1]) > 0);
        const coordinates = result.renderOps.flatMap(op => op.op === 'canvas' ? op.cmds.flatMap(command => command.slice(2).filter(value => typeof value === 'number')) : []);
        assert.ok(coordinates.length > 100);
        assert.ok(coordinates.every(Number.isFinite));
        assert.ok(result.canvasWordBatches > 0);
        assert.equal(result.timers, 0);
      }
    });

    it(`${file}: redraws without querying the host's media state`, async () => {
      const initial = await runWidget(html, { timerRounds: 0 });
      const repeated = await runWidget(html, { timerRounds: 0,
        probes: ['(draw(),draw(),draw(),status.textContent)'] });
      const mediaQueries = (queries: unknown[]) => queries.filter(query => (query as { q?: string }).q === 'matchMedia');
      assert.ok(mediaQueries(initial.queries).length > 0);
      assert.equal(mediaQueries(repeated.queries).length, mediaQueries(initial.queries).length);
      assert.ok(repeated.probes[0].length > 0);
      assert.deepEqual(repeated.diags, []);
      assert.deepEqual(repeated.logs.filter(log => log.level === 'error'), []);
    });

    it(`${file}: updates cached colors when the chat changes theme`, async () => {
      const snapshot = '(draw(),darkMode)';
      let mediaQueries = 0;
      const result = await runWidget(html, { theme: 'light', timerRounds: 0,
        answer: query => query.q === 'matchMedia' ? { matches: mediaQueries++ === 1 } : undefined, script: [
        { probe: snapshot }, { theme: 'dark' }, { probe: snapshot }, { theme: 'light' }, { probe: snapshot },
      ] });
      const [light, dark, restored] = result.probes.map(probe => JSON.parse(probe));
      assert.equal(mediaQueries, 3);
      assert.deepEqual([light, dark, restored], [false, true, false]);
      assert.deepEqual(result.diags, []);
      assert.deepEqual(result.logs.filter(log => log.level === 'error'), []);
    });

    it(`${file}: saves a changed model and zoom, then restores and resets them`, async () => {
      const initial = await runWidget(html);
      const id = (name: string) => {
        const op = initial.renderOps.find(op => op.op === 'el' && op.attrs.some(([key, value]) => key === 'id' && value === name));
        assert.ok(op);
        return op.id;
      };
      const changed = await runWidget(html, {
        events: ['special', 'near'].map(name => ({ type: 'click', targetId: id(name) })),
        probes: [state],
      });
      const modified = JSON.parse(changed.probes[0]);
      assert.equal(modified.variant, 1);
      assert.ok(Math.abs(modified.zoom - 1.1) < 1e-6);
      const storage = Object.assign({}, ...changed.storageCommits);
      assert.ok(Object.keys(storage.local).length > 0);
      const restored = await runWidget(html, { storage, probes: [state] });
      assert.deepEqual(JSON.parse(restored.probes[0]), modified);
      const reset = await runWidget(html, { storage,
        events: [{ type: 'click', targetId: id('reset') }], probes: [state] });
      const defaults = JSON.parse(reset.probes[0]);
      assert.ok(Math.abs(defaults.yaw + .55) < 1e-10);
      assert.deepEqual({ ...defaults, yaw: -.55 }, { yaw: -.55, pitch: .28, zoom: 1, variant: 0, drag: null });
    });

    it(`${file}: rolls back an orbit when the host cancels a pointer`, async () => {
      const initial = await runWidget(html);
      const canvas = initial.renderOps.find(op => op.op === 'el' && op.attrs.some(([key, value]) => key === 'id' && value === 'view'));
      assert.ok(canvas);
      const pointer = { targetId: canvas.id, pointerId: 1, pointerType: 'touch', button: 0 };
      const canceled = await runWidget(html, {
        events: [{ ...pointer, type: 'pointerdown', clientX: 120 },
          { ...pointer, type: 'pointermove', clientX: 180 },
          { ...pointer, type: 'pointercancel', clientX: 180 }], probes: [state],
      });
      assert.deepEqual(canceled.diags, []);
      assert.deepEqual(JSON.parse(canceled.probes[0]), { yaw: -.55, pitch: .28, zoom: 1, variant: 0, drag: null });
      assert.deepEqual(canceled.storageCommits, []);
    });
  }
});
