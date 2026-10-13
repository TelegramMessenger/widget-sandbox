import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';
import { Validator } from '../../src/host/validator.ts';

describe('scrollIntoView options', () => {
  it('preserves boolean and dictionary alignment and behavior', async () => {
    const r = await runWidget(`<div id=target></div><script>
      const el=document.getElementById('target');
      el.scrollIntoView();
      el.scrollIntoView(false);
      el.scrollIntoView(null);
      el.scrollIntoView({block:'center',inline:'end',behavior:'smooth',container:'nearest'});
      el.scrollIntoView({block:'nearest',inline:'center',behavior:'instant'});
    </script>`, { timerRounds: 0 });
    assert.deepEqual(r.renderOps.filter((op: any) => op.op === 'call' && op.method === 'scrollIntoView').map((op: any) => op.args), [[0,3,0,0],[2,3,0,0],[0,3,0,0],[1,2,2,1],[3,1,1,0]]);
  });

  it('rejects invalid enums before emitting a call', async () => {
    const r = await runWidget(`<div id=target></div><script>
      var errors=[];
      for (const o of [{block:'middle'},{inline:null},{behavior:'fast'},{container:'root'}]) {
        try { document.getElementById('target').scrollIntoView(o); } catch(e) { errors.push(e.name); }
      }
    </script>`, { timerRounds: 0, probes: ['JSON.stringify(errors)'] });
    assert.deepEqual(r.probes, ['["TypeError","TypeError","TypeError","TypeError"]']);
    assert.equal(r.renderOps.filter((op: any) => op.op === 'call').length, 0);
  });

  it('validates the wire enums and accepts legacy calls', () => {
    const v = new Validator(() => {});
    v.processOps([{op:'el',id:2,tag:'html'}, {op:'ins',id:2,parent:1}, {op:'el',id:3,tag:'body'}, {op:'ins',id:3,parent:2}]);
    const call = (args?: unknown) => v.processOps([{op:'call',id:3,method:'scrollIntoView',args}]);
    for (const args of [[4,3,0,0],[0,-1,0,0],[0,3,3,0],[0,3,0,2],[0.5,3,0,0],[0,3,NaN,0],['0',3,0,0],[0,3],null]) assert.equal(call(args).length, 0);
    for (const args of [undefined, []]) assert.deepEqual(call(args), [{op:'call',id:3,method:'scrollIntoView',args:[0,3,0,0]}]);
    assert.deepEqual(call([3,2,2,1]), [{op:'call',id:3,method:'scrollIntoView',args:[3,2,2,1]}]);
  });
});
