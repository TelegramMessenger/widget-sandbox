// Builds the JavaScript the native packages ship: the sandbox bundle, the
// renderer bundle (platform transport), the validator bundle, the bundled
// libraries, their precompiled bytecode and the shared test fixture — into
// packages/ios, packages/android or packages/desktop. The demo apps' corpus
// copy is a separate step (scripts/sync-demo-corpus.ts at the repo root).
//
// Usage: pnpm build:ios        (= pnpm -C packages/core build:native ios + the corpus sync)
//        pnpm build:macos      (same outputs as ios: one Swift package)
//        pnpm build:android
//        pnpm build:desktop

import { build } from 'esbuild';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { LIBRARIES } from '../src/host/loader.ts';
import { libraryPath, rendererBuildOptions, sandboxBuildOptions, validatorBuildOptions } from './sandbox-build.ts';

/** The repo root; the native packages are siblings of this one under packages/. */
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..', '..', '..');
/** This package (packages/core): the bundles' sources. */
const pkgRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');

const ios = { transport: 'webkit', js: 'packages/ios/Sources/WidgetSandbox/WidgetJS', fixtures: 'packages/ios/Tests/WidgetSandboxTests/Fixtures' } as const;
const TARGETS = {
  ios,
  // macOS uses the same Swift package, bundles (WKWebView transport) and demo corpus as iOS.
  macos: ios,
  // Desktop C++ core (desktop/): no renderer of its own yet; the renderer bundle is built for the
  // WKWebView-style bridge so a lib_webview-based host can pick it up unchanged on macOS.
  desktop: { transport: 'webkit', js: 'packages/desktop/WidgetJS', fixtures: 'packages/desktop/tests/fixtures' },
  android: { transport: 'android', js: 'packages/android/widget-sandbox/src/main/assets/WidgetJS', fixtures: 'packages/android/widget-sandbox/src/androidTest/assets' },
} as const;

const targetName = (process.argv[2] ?? 'ios') as keyof typeof TARGETS;
const target = TARGETS[targetName];
if (!target) {
  console.error(`unknown target "${targetName}"; expected one of ${Object.keys(TARGETS).join(', ')}`);
  process.exit(1);
}
const res = path.join(root, target.js);
fs.mkdirSync(path.join(res, 'libs'), { recursive: true });

async function emit(name: string, options: Parameters<typeof build>[0]) {
  const out = await build(options);
  const text = out.outputFiles![0].text;
  fs.writeFileSync(path.join(res, name), text);
  console.log(`${name}: ${(text.length / 1024).toFixed(0)} KB`);
}

await emit('sandbox-bundle.js', sandboxBuildOptions(pkgRoot));
await emit('renderer-bundle.js', rendererBuildOptions(pkgRoot, target.transport));
await emit('validator-bundle.js', validatorBuildOptions(pkgRoot));
for (const lib of LIBRARIES) {
  fs.copyFileSync(libraryPath(lib.source), path.join(res, 'libs', lib.file));
  console.log(`libs/${lib.file}`);
}

// Precompiled bytecode (.qbc next to each .js that QuickJS evaluates; the renderer bundle runs in
// a web view and has none). The compiler is built from the vendored quickjs-ng, so the bytecode
// always matches the engine the apps link; each blob carries the source's length and hash and is
// refused at load time if the .js next to it changed (see qjs_shim.h).
const qjsc = buildQjsc();
for (const name of ['sandbox-bundle.js', 'validator-bundle.js', ...LIBRARIES.map((l) => `libs/${l.file}`)]) {
  const out = name.replace(/\.js$/, '.qbc');
  execFileSync(qjsc, [path.join(res, name), path.join(res, out), path.basename(name)], { stdio: 'inherit' });
}

/** Builds packages/tools/qjsc from packages/ios/Sources/CQuickJS (the fork submodule + shim) into .cache/ (gitignored) when it is missing or older than its sources. */
function buildQjsc(): string {
  const qjs = path.join(root, 'packages/ios/Sources/CQuickJS');
  const sources = ['quickjs/quickjs.c', 'quickjs/dtoa.c', 'quickjs/libregexp.c', 'quickjs/libunicode.c', 'shim/qjs_shim.c'].map((f) => path.join(qjs, f));
  sources.push(path.join(root, 'packages/tools/qjsc/qjsc.c'));
  const headers = fs.readdirSync(path.join(qjs, 'quickjs')).filter((f) => f.endsWith('.h')).map((f) => path.join(qjs, 'quickjs', f));
  headers.push(path.join(qjs, 'include/qjs_shim.h'));
  const bin = path.join(root, '.cache', process.platform === 'win32' ? 'qjsc.exe' : 'qjsc');
  const newest = Math.max(...[...sources, ...headers].map((f) => fs.statSync(f).mtimeMs));
  if (fs.existsSync(bin) && fs.statSync(bin).mtimeMs > newest) return bin;
  fs.mkdirSync(path.dirname(bin), { recursive: true });
  console.log('building packages/tools/qjsc from the QuickJS submodule (TelegramMessenger/quickjs)…');
  const cc = process.env.CC ?? 'cc';
  execFileSync(cc, ['-O2', '-std=gnu11', '-D_GNU_SOURCE', '-DQUICKJS_NG_BUILD', '-I', path.join(qjs, 'include'), '-I', path.join(qjs, 'quickjs'),
    '-Wno-unused-parameter', ...sources, '-lm', '-o', bin], { stdio: 'inherit' });
  return bin;
}
// Library URL patterns, so the native loader resolves the same CDN URLs as the web host.
fs.writeFileSync(path.join(res, 'libraries.json'), JSON.stringify(LIBRARIES.map((l) => ({ name: l.name, file: l.file, patterns: l.patterns.map((p) => p.source) })), null, 2));

// Test fixtures shared with the native ports (one source of truth: packages/core/tests/fixtures).
fs.mkdirSync(path.join(root, target.fixtures), { recursive: true });
for (const f of ['cpu-budget.json']) fs.copyFileSync(path.join(pkgRoot, 'tests', 'fixtures', f), path.join(root, target.fixtures, f));
console.log(`fixtures copied to ${target.fixtures}`);
