// User activation for privileged bridge calls: the host-side policy shared by the
// web host (src/host/user-activation.ts) and the Node harness.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { GESTURE_TTL_MS, GestureCounter, HostActivationGate, isUserActivation } from '../../src/host/user-activation.ts';
import { runWidget } from '../../scripts/sandbox-node.ts';

describe('user activation: policy', () => {
  it('counts clicks and key presses, not Escape, repeats, Command shortcuts or moves', () => {
    assert.ok(isUserActivation({ type: 'click' }));
    assert.ok(isUserActivation({ type: 'keydown', key: 'a' }));
    assert.ok(!isUserActivation({ type: 'keydown', key: 'Escape' }));
    assert.ok(!isUserActivation({ type: 'keydown', key: 'a', repeat: true }));
    assert.ok(!isUserActivation({ type: 'keydown', key: 'c', modifiers: 4 }));
    assert.ok(!isUserActivation({ type: 'mousemove' }));
    assert.ok(!isUserActivation({ type: 'scroll' }));
  });

  it('numbers one physical press as one gesture', () => {
    const c = new GestureCounter();
    assert.deepEqual(['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'click', 'dblclick'].map((type) => c.gestureFor({ type })), [1, 1, 1, 1, 1, 1]);
    assert.equal(c.gestureFor({ type: 'pointerdown' }), 2);
    assert.equal(c.gestureFor({ type: 'keydown', key: 'a' }), 3);
    assert.equal(c.gestureFor({ type: 'keydown', key: 'b' }), 4);
    assert.equal(c.gestureFor({ type: 'mousemove' }), 0);
  });

  it('accepts each issued gesture once, while visible and recent, and nothing the host did not issue', () => {
    const g = new HostActivationGate();
    assert.ok(!g.accept(1, 0, true), 'not issued');
    g.issue(1, 0);
    assert.ok(!g.accept(1, 0, false), 'hidden');
    assert.ok(g.accept(1, 10, true));
    assert.ok(!g.accept(1, 10, true), 'consumed');
    g.issue(2, 100);
    assert.ok(!g.accept(2, 100 + GESTURE_TTL_MS + 1, true), 'stale');
    g.issue(3, 200);
    assert.ok(!g.accept('3', 200, true), 'not a number');
    assert.ok(!g.accept(99, 200, true), 'forged number');
    assert.ok(g.accept(3, 200, true));
    g.issue(4, 300);
    g.issue(5, 300);
    assert.ok(g.accept(5, 300, true));
    assert.ok(!g.accept(4, 300, true), 'an older gesture cannot be used after a newer one');
  });
});

describe('user activation: harness', () => {
  const widget = `<button id="b">go</button><script>
    TelegramWidget.ready();
    setTimeout(function () { TelegramWidget.sendMessage('timer'); }, 1);
    Promise.resolve().then(function () { TelegramWidget.openLink('https://example.com/boot'); });
    document.getElementById('b').addEventListener('click', function () { TelegramWidget.sendMessage('clicked'); TelegramWidget.sendMessage('again'); });
  </script>`;

  it('privileged calls outside a gesture are blocked; one trusted click unlocks one call', async () => {
    const first = await runWidget(widget, { timerRounds: 2, policy: 'strict' });
    const button = first.renderOps.find((o: any) => o.op === 'el' && o.tag === 'button') as any;
    const r = await runWidget(widget, { timerRounds: 2, policy: 'strict', events: [{ type: 'click', targetId: button.id }, { type: 'click', targetId: button.id, gesture: 1 }, { type: 'click', targetId: button.id, gesture: 1 }] });
    assert.deepEqual(r.bridge.map((b: any) => b[0] + ':' + JSON.stringify(b[1])), ['ready:[]', 'sendMessage:["clicked"]']);
    assert.ok(r.diags.filter((d) => d.kind === 'activation-blocked').length >= 4);
  });

  it('strict is the default, and anything but compat is strict', async () => {
    for (const policy of [undefined, 'Compat', 'compact'] as const) {
      const r = await runWidget(widget, { timerRounds: 2, policy: policy as 'compat' | undefined });
      assert.deepEqual(r.bridge.map((b: any) => b[0]), ['ready'], String(policy));
    }
  });

  it('the compat policy forwards every bridge call, as before the gate', async () => {
    const r = await runWidget(widget, { timerRounds: 2, policy: 'compat' });
    const calls = r.bridge.map((b: any) => b[0] + ':' + JSON.stringify(b[1]));
    assert.ok(calls.includes('sendMessage:["timer"]'), JSON.stringify(calls));
    assert.ok(calls.includes('openLink:["https://example.com/boot"]'), JSON.stringify(calls));
    assert.ok(!r.diags.some((d) => d.kind === 'activation-blocked'));
  });
});
