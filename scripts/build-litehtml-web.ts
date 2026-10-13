// Builds the litehtml renderer prototype for the web dev app: renderer-litehtml/
// (the core + vendored litehtml + the JS-backed C ABI in src/web/wasm.cpp) →
// Emscripten → renderer-litehtml/pkg-web/twidget_litehtml.{js,wasm}
// (gitignored; the Vite plugin serves it at /litehtml/, outside the module
// graph, like /blitz/). Objects go to renderer-litehtml/target/web-obj/ and are
// rebuilt only when their source is newer. Dev tooling, see
// docs/litehtml-renderer.md. Needs `emcc`/`em++` (brew install emscripten).
//
//   corepack pnpm build:litehtml-web

import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const root = path.join(path.dirname(new URL(import.meta.url).pathname), '..');
const core = path.join(root, 'apps', 'renderer-litehtml');
const lite = path.join(core, 'third_party', 'litehtml');
const outDir = path.join(core, 'pkg-web');
const objDir = path.join(core, 'target', 'web-obj');
const env = { ...process.env, PATH: ['/opt/homebrew/bin', '/usr/local/bin', process.env.PATH ?? ''].join(path.delimiter) };

const includes = [
  path.join(core, 'src'), path.join(root, 'packages', 'desktop', 'sandbox', 'include'),
  path.join(lite, 'include'), path.join(lite, 'include', 'litehtml'), path.join(lite, 'src'),
  path.join(lite, 'src', 'gumbo', 'include'), path.join(lite, 'src', 'gumbo', 'include', 'gumbo'),
].map((p) => `-I${p}`);
const cxxFlags = ['-O2', '-std=c++20', '-fwasm-exceptions', '-Wno-deprecated-declarations', ...includes];
const cFlags = ['-O2', '-std=gnu99', '-Wno-void-pointer-to-enum-cast', ...includes];

const sources = [
  ...['renderer.cpp', 'css.cpp', 'animations.cpp', 'canvas.cpp', 'compositor.cpp'].map((f) => path.join(core, 'src', f)),
  path.join(core, 'src', 'web', 'wasm.cpp'),
  ...fs.readdirSync(path.join(lite, 'src')).filter((f) => f.endsWith('.cpp')).map((f) => path.join(lite, 'src', f)),
  ...fs.readdirSync(path.join(lite, 'src', 'gumbo')).filter((f) => f.endsWith('.c')).map((f) => path.join(lite, 'src', 'gumbo', f)),
];
const exported = ['twl_create', 'twl_backend_id', 'twl_destroy', 'twl_receive', 'twl_set_width', 'twl_set_theme', 'twl_touch', 'twl_image_ready', 'twl_size', 'twl_dirty', 'twl_display_list', 'twl_stats', 'twl_html', 'twl_string_free', 'twl_alloc', 'twl_free'];

function objectFor(src: string) {
  return path.join(objDir, path.relative(root, src).replace(/[\\/]/g, '__') + '.o');
}

function stale(src: string, obj: string) {
  if (!fs.existsSync(obj)) return true;
  return fs.statSync(obj).mtimeMs < fs.statSync(src).mtimeMs;
}

function compile(src: string): Promise<void> {
  const obj = objectFor(src);
  const isC = src.endsWith('.c');
  const args = [...(isC ? cFlags : cxxFlags), '-c', src, '-o', obj];
  return new Promise((resolve, reject) => {
    const p = spawn(isC ? 'emcc' : 'em++', args, { cwd: core, env, stdio: 'inherit' });
    p.on('error', (e) => reject(new Error(`${isC ? 'emcc' : 'em++'}: ${e.message}${e.message.includes('ENOENT') ? ' (install it: brew install emscripten)' : ''}`)));
    p.on('exit', (code) => (code === 0 ? resolve() : reject(new Error(`${path.basename(src)}: exit ${code}`))));
  });
}

async function main() {
  fs.mkdirSync(objDir, { recursive: true });
  fs.mkdirSync(outDir, { recursive: true });
  const todo = sources.filter((s) => stale(s, objectFor(s)));
  console.log(`compiling ${todo.length} of ${sources.length} files`);
  const workers = Math.max(1, Math.min(os.cpus().length, 8));
  let next = 0;
  await Promise.all(Array.from({ length: workers }, async () => {
    while (next < todo.length) await compile(todo[next++]);
  }));
  const out = path.join(outDir, 'twidget_litehtml.js');
  const args = [
    '-O2', '-fwasm-exceptions',
    '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,node', '-sFILESYSTEM=0', '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=32MB',
    `-sEXPORTED_FUNCTIONS=${exported.map((f) => '_' + f).join(',')}`,
    '-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,HEAPU32,HEAPF32',
    '-o', out, ...sources.map(objectFor),
  ];
  console.log(`$ em++ ${args.slice(0, 9).join(' ')} … (${sources.length} objects)`);
  const r = spawnSync('em++', args, { cwd: core, env, stdio: 'inherit' });
  if (r.status !== 0) process.exit(r.status ?? 1);
  for (const f of fs.readdirSync(outDir).sort()) console.log(`${f}: ${(fs.statSync(path.join(outDir, f)).size / 1024 / 1024).toFixed(2)} MB`);
}

main().catch((e) => { console.error(e.message); process.exit(1); });
