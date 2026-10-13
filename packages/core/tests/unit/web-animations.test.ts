import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { describe, it } from 'node:test';
import { Validator } from '../../src/host/validator.ts';
import { runWidget, type RunOptions } from '../../scripts/sandbox-node.ts';

const here = path.dirname(new URL(import.meta.url).pathname);
const fixtures = path.join(here, '..', 'fixtures', 'waapi');
const appFixture = path.join(fixtures, 'app-anims.json');

function validator(diags: string[] = []): Validator {
  const v = new Validator((kind, detail) => diags.push(`${kind}: ${detail}`));
  v.processOps([
    { op: 'el', id: 2, tag: 'html' }, { op: 'ins', parent: 1, id: 2, before: null },
    { op: 'el', id: 3, tag: 'body' }, { op: 'ins', parent: 2, id: 3, before: null },
    { op: 'el', id: 7, tag: 'div' }, { op: 'ins', parent: 3, id: 7, before: null },
  ]);
  return v;
}

const frames = (values: Record<string, string>[] = [{ opacity: '0' }, { opacity: '1' }]) =>
  values.map((v, i) => ({ offset: values.length === 1 ? 1 : i / (values.length - 1), easing: 'linear', composite: null, values: v }));

function record(over: Record<string, unknown> = {}, timing: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    aid: 1, target: 7, keyframes: frames(),
    timing: { duration: 1000, delay: 0, endDelay: 0, iterations: null, iterationStart: 0, direction: 'normal', fill: 'none', easing: 'ease-in', ...timing },
    composite: 'replace', iterationComposite: 'replace', startTime: 1000, holdTime: null, playbackRate: 1, ...over,
  };
}

const clock = () => { let t = 1000; return () => (t += 16); };
const bytes = () => { let s = 7; return (n: number) => Array.from({ length: n }, () => (s = (s * 1103515245 + 12345) & 0xff)); };

interface Case { name: string; html: string; opts?: RunOptions }

const cssList = [{ kind: 'animation', id: 5, name: 'spin', index: 0, origin: 3, currentTime: 250, playbackRate: 1, paused: false, duration: 1000, delay: 0, iterations: null, direction: 'normal', fill: 'none', easing: 'linear' }];

const inline: Case[] = [
  {
    name: 'pending-then-started',
    html: "<div id=a></div><script>const a=document.getElementById('a').animate({opacity:[0,1],backgroundColor:['red','blue']},{duration:500,iterations:Infinity,easing:'steps(2)'});a.ready.then(()=>console.log('ready',a.playState,a.startTime!==null));</script>",
  },
  {
    name: 'keyframe-list',
    html: "<div id=a>x</div><script>const a=document.getElementById('a').animate([{opacity:0,transform:'translateX(0px)'},{opacity:1,transform:'translateX(100px)',offset:0.7,easing:'cubic-bezier(0.1, 0.2, 0.3, 1)'},{opacity:0.5}],{duration:500,iterations:2,easing:'ease-in',direction:'alternate',fill:'both',delay:20});a.onfinish=()=>console.log('finish');console.log(typeof Animation,typeof KeyframeEffect,document.getAnimations().length,a.effect.getKeyframes().length);</script>",
  },
  {
    name: 'control',
    html: "<div id=a></div><script>const a=document.getElementById('a').animate({transform:['none','rotate(90deg)']},{duration:400,composite:'add'});a.pause();a.currentTime=100;a.playbackRate=2;a.play();a.updatePlaybackRate(-1);a.ready.then(()=>{console.log('r',a.playState,a.playbackRate);a.finish();console.log('f',a.playState,a.currentTime);});</script>",
  },
  {
    name: 'effect-objects',
    html: "<div id=a></div><div id=b></div><script>const e=new KeyframeEffect(document.getElementById('a'),{opacity:[1,0]},{duration:300,iterationStart:0.5,endDelay:10});const x=new Animation(e,document.timeline);x.play();e.target=document.getElementById('b');e.updateTiming({easing:'linear(0, 0.5 25%, 1)'});x.ready.then(()=>{console.log(x.playState,e.getComputedTiming().activeDuration);x.cancel();console.log(x.playState);});</script>",
  },
  {
    name: 'shim-rejects',
    html: "<div id=a></div><script>const el=document.getElementById('a');const t=(f)=>{try{f();console.log('ok')}catch(e){console.log(e.name,String(e.message).slice(0,40))}};t(()=>el.animate({opacity:[0,1]},{duration:NaN}));t(()=>el.animate({opacity:[0,1]},{duration:-1}));t(()=>el.animate({opacity:[0,1]},{delay:Infinity}));t(()=>el.animate({opacity:[0,1]},{easing:'evil(1)'}));t(()=>el.animate([{opacity:0,offset:0.8},{opacity:1,offset:0.2}],100));t(()=>el.animate(Array.from({length:10001},()=>({opacity:1})),100));t(()=>el.animate({opacity:[0,1],animationName:['a','b'],transitionDuration:['1s','2s'],willChange:['x','y']},100));</script>",
  },
  {
    name: 'css-objects',
    html: "<div id=a></div><script>const l=document.getElementById('a').getAnimations();console.log(l.length,l[0]&&l[0].constructor.name,l[0]&&l[0].animationName,l[0]&&l[0].currentTime);if(l[0]){l[0].pause();l[0].currentTime=500;}</script>",
    opts: { answer: (q: any) => (q.q === 'anims' ? { list: cssList } : null) },
  },
];

function cases(): Case[] {
  const files = fs.readdirSync(fixtures).filter((f) => f.endsWith('.html')).sort();
  return [...inline, ...files.map((f) => ({ name: f, html: fs.readFileSync(path.join(fixtures, f), 'utf8') }))];
}

async function capture(c: Case, bundle?: string) {
  const r = await runWidget(c.html, { ...c.opts, ...(bundle ? { bundle } : { webAnimations: true }), now: clock(), random: bytes(), frames: 6, timerRounds: 4 });
  return { anims: r.anims, animOps: r.animOps, logs: r.logs.map((l) => `${l.level}:${l.text}`) };
}

describe('web animations: differential against the app bundle', () => {
  const appBundle = process.env.TW_APP_SANDBOX_BUNDLE;
  it('the shim sends the same anim batches, validated records and logs as the app bundle', async () => {
    if (appBundle) {
      const bundle = fs.readFileSync(appBundle, 'utf8');
      const out: Record<string, unknown> = {};
      for (const c of cases()) out[c.name] = await capture(c, bundle);
      fs.writeFileSync(appFixture, JSON.stringify(out, null, 1) + '\n');
    }
    const expected = JSON.parse(fs.readFileSync(appFixture, 'utf8'));
    for (const c of cases()) {
      assert.ok(expected[c.name], `fixture lacks ${c.name}`);
      const got = await capture(c);
      assert.ok(got.anims.length > 0 || c.name === 'shim-rejects', `${c.name}: no anim batches`);
      assert.deepEqual(got, expected[c.name], c.name);
    }
  });
});

describe('web animations: shim', () => {
  it('reports unsupported and sends nothing without the feature', async () => {
    const r = await runWidget("<div id=a></div><script>const a=document.getElementById('a').animate({opacity:[0,1]},100);console.log(typeof Animation,a.playState,document.getAnimations().length);</script>");
    assert.equal(r.anims.length, 0);
    assert.ok(r.diags.some((d) => d.kind === 'unsupported' && /Element\.animate/.test(d.detail)));
    assert.deepEqual(r.logs.map((l) => l.text), ['undefined finished 0']);
  });

  it('sends a pending record with a hold time, then a started one with a start time', async () => {
    const r = await runWidget(inline[0].html, { webAnimations: true, now: clock() });
    const pending = (r.anims[0] as any[])[0];
    assert.equal(pending.holdTime, 0);
    assert.equal(pending.startTime, null);
    assert.equal(pending.timing.iterations, null);
    assert.equal(pending.timing.easing, 'steps(2)');
    assert.equal(pending.keyframes.at(-1).values['background-color'], 'blue');
    const started = (r.anims.at(-1) as any[])[0];
    assert.equal(started.holdTime, null);
    assert.equal(typeof started.startTime, 'number');
    assert.ok(r.logs.some((l) => l.text === 'ready running true'));
  });

  it('never sends animation or transition properties and throws on bad timing', async () => {
    const r = await runWidget(inline[4].html, { webAnimations: true, now: clock() });
    const texts = r.logs.map((l) => l.text);
    assert.deepEqual(texts.slice(0, 6).map((t) => t.split(' ')[0]), ['TypeError', 'TypeError', 'TypeError', 'TypeError', 'TypeError', 'RangeError']);
    for (const batch of r.anims) for (const a of batch as any[]) for (const f of a.keyframes ?? []) {
      for (const k of Object.keys(f.values)) assert.ok(!/^(animation|transition|will-change)/.test(k), k);
    }
  });
});

describe('web animations: validateAnim', () => {
  it('passes clean records and sanitizes values', () => {
    const v = validator();
    const kf = [
      { offset: 0, easing: 'linear', composite: null, values: { opacity: '0', 'background-image': 'url(https://example.com/x.png)', 'animation-name': 'x', transition: 'all 1s', color: 'light-dark(red, blue)' } },
      { offset: 1, easing: 'steps(2, jump-none)', composite: 'add', values: { transform: 'translateX(10px)' } },
    ];
    const out = v.validateAnim([record({ keyframes: kf })]) as any[];
    assert.equal(out.length, 1);
    assert.deepEqual(out[0].keyframes[0].values, { opacity: '0', color: 'red' });
    assert.equal(out[0].playbackRate, 1);
  });

  it('rejects hostile records with diagnostics', () => {
    const many = Array.from({ length: 65 }, (_, i) => ({ offset: i / 64, easing: 'linear', composite: null, values: {} }));
    const rejected: unknown[] = [
      record({ target: 99 }),
      record({ keyframes: many }),
      record({}, { duration: 1e12 }),
      record({}, { duration: NaN }),
      record({}, { duration: Infinity }),
      record({}, { delay: -Infinity }),
      record({}, { iterations: NaN }),
      record({}, { iterations: -1 }),
      record({}, { easing: 'evil(1)' }),
      record({}, { easing: 'linear(' + Array(300).fill('0').join(',') + ')' }),
      record({}, { easing: 'cubic-bezier(0, 0, 1, url(x))' }),
      record({}, { direction: 'sideways' }),
      record({}, { fill: 'auto' }),
      record({ playbackRate: 1e6 }),
      record({ playbackRate: NaN }),
      record({ startTime: Infinity }),
      record({ startTime: null, holdTime: null }),
      record({ composite: 'multiply' }),
      record({ keyframes: [{ offset: 0.8, easing: 'linear', composite: null, values: {} }, { offset: 0.2, easing: 'linear', composite: null, values: {} }] }),
      record({ keyframes: [{ offset: NaN, easing: 'linear', composite: null, values: {} }] }),
      record({ keyframes: [{ offset: 1.5, easing: 'linear', composite: null, values: {} }] }),
      record({ keyframes: [{ offset: 0, easing: 'linear', composite: null, values: Object.fromEntries(Array.from({ length: 65 }, (_, i) => [`--v${i}`, '1'])) }] }),
      record({ keyframes: 'opacity' }),
      record({ timing: null }),
      { aid: -1 },
      { aid: 1.5, remove: true },
      { aid: 2 ** 31 },
      null,
      { aid: 2, target: 7, css: { kind: 'animation', name: 'a;b', origin: 1 }, startTime: null, holdTime: 0, playbackRate: 1, cancelled: false },
      { aid: 2, target: 7, css: { kind: 'keyframes', name: 'a', origin: 1 }, startTime: null, holdTime: 0, playbackRate: 1, cancelled: false },
      { aid: 2, target: 7, css: { kind: 'animation', name: 'a', origin: NaN }, startTime: null, holdTime: 0, playbackRate: 1, cancelled: false },
    ];
    for (const r of rejected) {
      const diags: string[] = [];
      const v = validator(diags);
      assert.deepEqual(v.validateAnim([r]), [], JSON.stringify(r));
      if (!(r && typeof r === 'object' && (r as any).target === 99)) assert.ok(diags.some((d) => d.startsWith('protocol-violation')), JSON.stringify(r) + ' ' + diags.join('|'));
    }
    const diags: string[] = [];
    assert.deepEqual(validator(diags).validateAnim({ not: 'a list' }), []);
    assert.deepEqual(validator(diags).validateAnim(Array(4097).fill(record())), []);
    assert.equal(diags.filter((d) => d.startsWith('protocol-violation')).length, 2);
  });

  it('drops url(), expression() and non-allowlisted properties from keyframe values', () => {
    const diags: string[] = [];
    const values = {
      'background-image': 'url(javascript:alert(1))', background: 'url(https://evil.example/x) red', 'list-style': 'url(x.png)',
      width: 'expression(alert(1))', color: 'red; background: url(x)', '--x': 'url(https://evil.example/)', 'mask-image': 'image-set(url(x) 1x)',
      behavior: 'url(x.htc)', '-moz-binding': 'url(x.xml#y)', 'Opacity': '1', 'transition-duration': '1s', '-webkit-animation-name': 'x', ['a'.repeat(70)]: '1',
      filter: 'url(#f)', cursor: 'url(x.cur), auto', content: 'attr(href)', height: 'calc(1px + var(--y))', opacity: '0.5',
    };
    const out = validator(diags).validateAnim([record({ keyframes: [{ offset: 0, easing: 'linear', composite: null, values }] })]) as any[];
    assert.equal(out.length, 1);
    const kept = out[0].keyframes[0].values;
    for (const [k, val] of Object.entries(kept)) assert.ok(!/url\s*\((?!#)|expression|javascript:/i.test(String(val)), `${k}: ${val}`);
    for (const k of ['behavior', '-moz-binding', 'Opacity', 'transition-duration', '-webkit-animation-name', 'a'.repeat(70), 'width', 'background-image']) assert.ok(!(k in kept), k);
    assert.equal(kept.opacity, '0.5');
    assert.ok(diags.length > 0);
  });

  it('caps live animations and forwards removals once', () => {
    const diags: string[] = [];
    const v = validator(diags);
    const batch = Array.from({ length: 1100 }, (_, i) => record({ aid: i + 1 }));
    assert.equal(v.validateAnim(batch).length, 1024);
    assert.ok(diags.some((d) => d.startsWith('animation-limit')));
    assert.deepEqual(v.validateAnim([{ aid: 5, remove: true }]), [{ aid: 5, remove: true }]);
    assert.deepEqual(v.validateAnim([{ aid: 5, remove: true }]), []);
    assert.equal(v.validateAnim([record({ aid: 2000 })]).length, 1);
    assert.deepEqual(v.validateAnim([record({ aid: 3, keyframes: [{ offset: 2 }] })]), [{ aid: 3, remove: true }]);
  });

  it('accepts the anims query only for rendered elements', () => {
    const v = validator();
    assert.deepEqual(v.validateQuery({ q: 'anims', id: 7, subtree: true }), { q: 'anims', id: 7, subtree: true });
    assert.deepEqual(v.validateQuery({ q: 'anims', id: 7, subtree: 'yes' }), { q: 'anims', id: 7, subtree: false });
    assert.equal(v.validateQuery({ q: 'anims', id: 99 }), null);
  });
});
