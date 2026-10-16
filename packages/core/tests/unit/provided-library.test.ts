import assert from 'node:assert/strict';
import { afterEach, describe, it } from 'node:test';
import { clearProvidedLibraries, provideLibrary, resolveLibrary } from '../../src/host/loader.ts';
import { runWidget } from '../../scripts/sandbox-node.ts';

const THREE = {
  name: 'three',
  file: 'three.js',
  patterns: [
    String.raw`app:three`,
    String.raw`https?://cdn\.jsdelivr\.net/npm/three@0\.1[5-6]\d(?:\.\d+)?/build/three(?:\.min)?\.js`,
    String.raw`https?://unpkg\.com/three@0\.1[5-6]\d(?:\.\d+)?/build/three(?:\.min)?\.js`,
  ],
};

describe('provided libraries (loader)', () => {
  afterEach(() => clearProvidedLibraries());

  it('resolves app: and CDN URLs only after a host provides the library', () => {
    assert.equal(resolveLibrary('app:three'), null);
    assert.ok(provideLibrary(THREE));
    for (const url of ['app:three', 'APP:THREE', ' app:three ', 'https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js', '//unpkg.com/three@0.158.0/build/three.js?v=1#x']) {
      assert.equal(resolveLibrary(url)?.name, 'three', url);
    }
    assert.equal(resolveLibrary('https://cdn.jsdelivr.net/npm/d3@7')?.name, 'd3');
  });

  it('does not resolve spoofed URLs', () => {
    provideLibrary(THREE);
    for (const url of [
      'https://evil.example/three@0.160.0/build/three.min.js',
      'https://cdn.jsdelivr.net.evil.example/npm/three@0.160.0/build/three.min.js',
      'https://cdn.jsdelivr.net@evil.example/npm/three@0.160.0/build/three.min.js',
      'https://evil.example/x.js?https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js',
      'https://cdn.jsdelivr.net/npm/three@0.160.0/build/../../evil/x.js',
      'https://cdn.jsdelivr.net/npm/three@0.199.0/build/three.min.js',
      'https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js\nhttps://evil.example/x.js',
      'app:three/../x', 'app:threes', 'xapp:three', 'app:three\u0000', 'javascript:THREE',
      'https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js?' + 'a'.repeat(3000),
    ]) assert.equal(resolveLibrary(url), null, url);
  });

  it('refuses unsafe registrations', () => {
    for (const bad of [
      { ...THREE, name: 'd3' }, { ...THREE, file: 'd3.min.js' }, { ...THREE, file: '../x.js' }, { ...THREE, file: 'x.txt' }, { ...THREE, name: 'A B' },
      { ...THREE, patterns: [] }, { ...THREE, patterns: ['.*'] }, { ...THREE, patterns: ['app:x|.*'] }, { ...THREE, patterns: ['https?://.*'] },
      { ...THREE, patterns: ['('] }, { ...THREE, patterns: ['app:x|'] }, { ...THREE, patterns: ['x'.repeat(2000)] },
      // An unbalanced `)` would close the anchoring group: `^(?:p)|(q)$` matches any URL that starts with p.
      { ...THREE, patterns: [String.raw`https://cdn\.example\.com/lib)|(zzz`] }, { ...THREE, patterns: ['app:x)|(.*'] }, { ...THREE, patterns: ['a)(b'] },
      { ...THREE, patterns: ['app:x\\'] },
    ]) assert.equal(provideLibrary(bad as any), false, JSON.stringify(bad).slice(0, 80));
    assert.equal(resolveLibrary('https://cdn.example.com/lib-evil.js'), null);
    assert.ok(provideLibrary({ name: 'x', file: 'x.js', patterns: ['app:x'] }));
    assert.equal(provideLibrary({ name: 'y', file: 'x.js', patterns: ['app:y'] }), false);
    assert.equal(resolveLibrary('app:x/y'), null);
    assert.ok(provideLibrary({ name: 'z', file: 'z.js', patterns: [String.raw`app:z|https://cdn\.example\.com/z(?:\.min)?\.js`, String.raw`app:\(z\)|[)(]z`] }), 'alternatives and escaped or bracketed parentheses stay inside the group');
    for (const url of ['app:z', 'https://cdn.example.com/z.min.js', 'app:(z)', ')z']) assert.equal(resolveLibrary(url)?.name, 'z', url);
    for (const url of ['app:zz', 'https://cdn.example.com/z.js/x', 'xapp:z']) assert.equal(resolveLibrary(url), null, url);
  });

  it('a widget loads the provided copy and gets nothing for a spoofed CDN URL', async () => {
    const libraries = [{ spec: THREE, source: "globalThis.THREE = { REVISION: '160' };" }];
    const ok = await runWidget("<script src=\"app:three\"></script><script>console.log('three:' + THREE.REVISION)</script>", { libraries });
    assert.ok(ok.logs.some((l) => l.text === 'three:160'), JSON.stringify(ok.logs));
    const spoof = await runWidget("<script src=\"https://cdn.jsdelivr.net.evil.example/npm/three@0.160.0/build/three.min.js\"></script><script>console.log('three:' + typeof THREE)</script>", { libraries });
    assert.ok(spoof.logs.some((l) => l.text === 'three:undefined'));
    assert.ok(spoof.diags.some((d) => d.kind === 'script-skipped'));
    assert.equal(resolveLibrary('app:three'), null);
  });
});
