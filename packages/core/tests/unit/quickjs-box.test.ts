// packages/quickjs-box, the QuickJS engine inside a wasm2c box that a native client may opt
// into: what is committed (the wasm, the C generated from it, WABT's runtime) is what
// manifest.sha256 lists, it was built from the submodule the native packages compile and from
// the guest layer and the shim as they are now, and the files a client compiles include it
// under the box's one configuration. The rebuild itself
// (emcc, wasm2c at the pinned commit) is `packages/quickjs-box/build.sh --check`.

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..', '..');
const box = path.join(root, 'packages/quickjs-box');
const sha256 = (file: string) => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const files = (dir: string) => fs.readdirSync(path.join(box, dir)).filter((f) => fs.statSync(path.join(box, dir, f)).isFile()).sort();

describe('packages/quickjs-box', () => {
  it('is exactly what manifest.sha256 lists (rebuild with packages/quickjs-box/build.sh)', () => {
    const listed = fs.readFileSync(path.join(box, 'manifest.sha256'), 'utf8').trim().split('\n').map((l) => {
      const m = /^([0-9a-f]{64}) {2}(\S+)$/.exec(l);
      assert.ok(m, `manifest.sha256: ${l}`);
      return [m[2], m[1]] as const;
    });
    const present = ['generated', 'wabt', 'wasm'].flatMap((dir) => files(dir).map((f) => `${dir}/${f}`));
    assert.deepEqual(listed.map(([f]) => f), present);
    for (const [f, sum] of listed) assert.equal(sha256(path.join(box, f)), sum, f);
  });

  it('was built from the QuickJS submodule the native packages compile (wasm/QUICKJS_VERSION)', (t) => {
    let commit: string;
    try {
      commit = execFileSync('git', ['-C', path.join(root, 'packages/ios/Sources/CQuickJS/quickjs'), 'rev-parse', 'HEAD'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
    } catch {
      t.skip('the submodule is not a git checkout');
      return;
    }
    assert.equal(fs.readFileSync(path.join(box, 'wasm/QUICKJS_VERSION'), 'utf8').trim(), commit.slice(0, 7));
  });

  it('was built from the guest layer and the shim as they are now (wasm/SOURCES.sha256)', () => {
    const lines = fs.readFileSync(path.join(box, 'wasm/SOURCES.sha256'), 'utf8').trim().split('\n');
    assert.deepEqual(lines.map((l) => l.slice(66)), ['packages/quickjs-box/guest/qjsbox_guest.c', 'packages/ios/Sources/CQuickJS/shim/qjs_shim.c', 'packages/ios/Sources/CQuickJS/include/qjs_shim.h']);
    for (const l of lines) assert.equal(sha256(path.join(root, l.slice(66))), l.slice(0, 64), `${l.slice(66)} changed: run packages/quickjs-box/build.sh`);
  });

  it('says in generated/qjsbox_build.h what it is: the engine commit, the wasm, the pinned tools', () => {
    const h = fs.readFileSync(path.join(box, 'generated/qjsbox_build.h'), 'utf8');
    const define = (name: string) => new RegExp(`^#define ${name} "?([^"\\n]+)"?$`, 'm').exec(h)?.[1];
    assert.equal(define('QJSBOX_QUICKJS_COMMIT'), fs.readFileSync(path.join(box, 'wasm/QUICKJS_VERSION'), 'utf8').trim());
    assert.equal(define('QJSBOX_WASM_SHA256'), sha256(path.join(box, 'wasm/qjsbox.wasm')));
    const script = fs.readFileSync(path.join(box, 'build.sh'), 'utf8');
    const pin = (name: string) => new RegExp(`^${name}=(\\S+)$`, 'm').exec(script)?.[1];
    assert.equal(define('QJSBOX_EMSCRIPTEN_VERSION'), pin('EMSDK_VERSION'));
    assert.equal(define('QJSBOX_WABT_COMMIT'), pin('WABT_COMMIT'));
    assert.match(pin('WABT_COMMIT') ?? '', /^[0-9a-f]{40}$/, 'WABT is pinned by commit');
    // The web's engine and the box are built with the same emscripten.
    const web = fs.readFileSync(path.join(root, 'packages/quickjs-wasm/build.sh'), 'utf8');
    assert.equal(pin('EMSDK_VERSION'), /^EMSDK_VERSION=(\S+)$/m.exec(web)?.[1]);
    assert.equal(pin('EMSDK_IMAGE'), /^EMSDK_IMAGE=(\S+)$/m.exec(web)?.[1]);
  });

  it('is compiled only through src/, every unit after qjsbox_config.h', () => {
    const units = Number(/^#define QJSBOX_NUM_OUTPUTS (\d+)$/m.exec(fs.readFileSync(path.join(box, 'generated/qjsbox_build.h'), 'utf8'))?.[1]);
    const generated = files('generated').filter((f) => /^qjsbox_\d+\.c$/.test(f));
    assert.equal(generated.length, units);
    const included = new Set<string>();
    for (const f of files('src').filter((f) => f.endsWith('.c'))) {
      const text = fs.readFileSync(path.join(box, 'src', f), 'utf8');
      const first = /^#include "([^"]+)"/m.exec(text)?.[1];
      assert.equal(first, 'qjsbox_config.h', `${f}: the configuration comes first`);
      for (const m of text.matchAll(/^#include "\.\.\/(generated|wabt)\/([^"]+)"/gm)) included.add(`${m[1]}/${m[2]}`);
    }
    assert.deepEqual([...included].sort(), [...generated.map((f) => `generated/${f}`), 'wabt/wasm-rt-impl.c', 'wabt/wasm-rt-mem-impl.c'].sort());
    // "Contain and crash": no signal handler of WABT's, no unwinding, the configuration in one place.
    const config = fs.readFileSync(path.join(box, 'src/qjsbox_config.h'), 'utf8');
    assert.match(config, /^#define WASM_RT_SKIP_SIGNAL_RECOVERY 1$/m);
    assert.match(config, /^#define WASM_RT_TRAP_HANDLER qjsbox_wasm_trap$/m);
    const host = fs.readFileSync(path.join(box, 'src/qjsbox_host.c'), 'utf8');
    assert.doesNotMatch(host, /\b(sigaction|signal|setjmp|longjmp|wasm_rt_impl_try)\s*\(/);
  });

  it('imports nothing but the host functions build.sh allows', () => {
    const h = fs.readFileSync(path.join(box, 'generated/qjsbox.h'), 'utf8');
    const imports = [...h.matchAll(/^\/\* import: '([^']+)' '([^']+)' \*\/$/gm)].map((m) => `${m[1]}.${m[2]}`).sort();
    const script = fs.readFileSync(path.join(box, 'build.sh'), 'utf8');
    const allowed = /^IMPORTS="([^"]+)"$/m.exec(script)?.[1].split(/\s+/).sort();
    assert.deepEqual(imports, allowed);
    assert.equal(imports.length, 11);
    const host = fs.readFileSync(path.join(box, 'src/qjsbox_host.c'), 'utf8');
    for (const name of imports) {
      const symbol = `w2c_${name.replace('wasi_snapshot_preview1.', 'wasi__snapshot__preview1_').replace('.', '_')}`;
      assert.ok(new RegExp(`^(u32|void) ${symbol}\\(`, 'm').test(host), `${symbol} is implemented in src/qjsbox_host.c`);
    }
  });
});
