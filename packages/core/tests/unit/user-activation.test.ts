// User activation for privileged bridge calls: the host-side policy shared by the
// web host (src/host/user-activation.ts) and the Node harness.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { GESTURE_TTL_MS, GestureCounter, HostActivationGate, isUserActivation } from '../../src/host/user-activation.ts';
import { runWidget } from '../../scripts/sandbox-node.ts';

describe('user activation: policy', () => {
  it('counts clicks and presses of Enter or Space, not other keys, repeats, Command shortcuts or moves', () => {
    assert.ok(isUserActivation({ type: 'click' }));
    assert.ok(isUserActivation({ type: 'keydown', key: 'Enter' }));
    assert.ok(isUserActivation({ type: 'keydown', key: ' ' }));
    for (const key of ['a', 'Escape', 'Tab', 'ArrowDown', 'PageDown', 'Shift', 'F5', 'Spacebar', 'enter', undefined]) assert.ok(!isUserActivation({ type: 'keydown', key }), String(key));
    assert.ok(!isUserActivation({ type: 'keydown', key: 'Enter', repeat: true }));
    assert.ok(!isUserActivation({ type: 'keydown', key: 'Enter', modifiers: 4 }));
    assert.ok(!isUserActivation({ type: 'mousemove' }));
    assert.ok(!isUserActivation({ type: 'scroll' }));
  });

  it('numbers one physical press as one gesture', () => {
    const c = new GestureCounter();
    assert.deepEqual(['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'click', 'dblclick'].map((type) => c.gestureFor({ type })), [1, 1, 1, 1, 1, 1]);
    assert.equal(c.gestureFor({ type: 'pointerdown' }), 2);
    assert.equal(c.gestureFor({ type: 'keydown', key: 'Enter' }), 3);
    assert.equal(c.gestureFor({ type: 'keydown', key: ' ' }), 4);
    assert.equal(c.gestureFor({ type: 'keydown', key: 'Tab' }), 0);
    assert.equal(c.gestureFor({ type: 'mousemove' }), 0);
  });

  it('an Enter or Space press and the click it activates are one gesture, also while the key is held', () => {
    const c = new GestureCounter();
    assert.equal(c.gestureFor({ type: 'keydown', key: 'Enter' }), 1);
    assert.equal(c.gestureFor({ type: 'click', detail: 0 }), 1, 'the click Enter activates');
    assert.equal(c.gestureFor({ type: 'keydown', key: 'Enter', repeat: true }), 0);
    assert.equal(c.gestureFor({ type: 'click', detail: 0 }), 1, 'the click of a held key');
    assert.equal(c.gestureFor({ type: 'keydown', key: ' ' }), 2);
    assert.equal(c.gestureFor({ type: 'keyup', key: ' ' }), 0);
    assert.equal(c.gestureFor({ type: 'click', detail: 0 }), 2, 'Space clicks on keyup');
    assert.equal(c.gestureFor({ type: 'click', detail: 0 }), 3, 'a second keyboard click without a key press is a new gesture');
    assert.equal(c.gestureFor({ type: 'keydown', key: 'Enter' }), 4);
    assert.equal(c.gestureFor({ type: 'click', detail: 1 }), 5, 'a pointer click after a key press is its own gesture');
    assert.equal(c.gestureFor({ type: 'keydown', key: 'Enter' }), 6);
    assert.equal(c.gestureFor({ type: 'keydown', key: 'Tab' }), 0);
    assert.equal(c.gestureFor({ type: 'click', detail: 0 }), 6, 'keys that are not a gesture do not close the press');
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

  it('a hidden widget cannot use a gesture', async () => {
    const first = await runWidget(widget, { timerRounds: 2, policy: 'strict' });
    const button = first.renderOps.find((o: any) => o.op === 'el' && o.tag === 'button') as any;
    const hidden = await runWidget(widget, { timerRounds: 2, policy: 'strict', visible: false, events: [{ type: 'click', targetId: button.id, gesture: 1 }] });
    assert.deepEqual(hidden.bridge.map((b: any) => b[0]), ['ready']);
    assert.ok(hidden.diags.some((d) => d.kind === 'activation-blocked' && d.detail.startsWith('sendMessage')));
    const shown = await runWidget(widget, { timerRounds: 2, policy: 'strict', events: [{ type: 'click', targetId: button.id, gesture: 1 }] });
    assert.deepEqual(shown.bridge.map((b: any) => b[0]), ['ready', 'sendMessage']);
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
