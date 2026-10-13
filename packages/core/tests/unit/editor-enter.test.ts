import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';

async function enter(handler = '', modifiers = 0) {
  const html = `<form><textarea id=t>hello</textarea></form><script>
var seen=[];var t=document.getElementById('t');
document.querySelector('form').addEventListener('submit',e=>{e.preventDefault();seen.push('submit')});
for(var type of ['keydown','keypress','keyup'])t.addEventListener(type,e=>seen.push(e.type+':'+e.shiftKey));
${handler}</script>`;
  const first = await runWidget(html, { timerRounds: 0 });
  const target = first.renderOps.find((op: any) => op.op === 'el' && op.tag === 'textarea') as any;
  const result = await runWidget(html, { timerRounds: 0, events: [{ type: 'keydown', targetId: target.id, key: 'Enter', code: 'Enter', modifiers, editorGeneration: 4, editorCommand: 7 }], probes: ['JSON.stringify(seen)'] });
  const completion = result.renderOps.find((op: any) => op.op === 'call' && op.method === 'completeEditorCommand') as any;
  return { result, completion };
}

describe('native textarea Enter', () => {
  it('cancels the first Enter without inserting a newline when a composer submits', async () => {
    const { result, completion } = await enter(`t.addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();t.form.requestSubmit()}})`);
    assert.deepEqual(result.probes, ['["keydown:false","submit"]']);
    assert.deepEqual(completion.args, [4, 7, 0]);
  });

  it('allows Shift+Enter and dispatches keypress before the native default', async () => {
    const { result, completion } = await enter(`t.addEventListener('keydown',e=>{if(!e.shiftKey){e.preventDefault();t.form.requestSubmit()}})`, 8);
    assert.deepEqual(result.probes, ['["keydown:true","keypress:true"]']);
    assert.deepEqual(completion.args, [4, 7, 1]);
  });

  it('honors keypress cancellation on the first press', async () => {
    const { result, completion } = await enter(`t.addEventListener('keypress',e=>e.preventDefault())`);
    assert.deepEqual(result.probes, ['["keydown:false","keypress:false"]']);
    assert.deepEqual(completion.args, [4, 7, 0]);
  });

  it('applies handler value changes before completing the native command', async () => {
    const { result, completion } = await enter(`t.addEventListener('keydown',()=>{t.value='replacement'})`);
    const property = result.renderOps.findIndex((op: any) => op.op === 'prop' && op.name === 'value' && op.value === 'replacement');
    assert.ok(property >= 0);
    assert.ok(property < result.renderOps.indexOf(completion), JSON.stringify(result.renderOps));
    assert.deepEqual(completion.args, [4, 7, 1]);
  });
});
