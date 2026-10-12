// esbuild options for the in-sandbox bundle, shared by the Vite plugin
// (packages/core/vite.ts) and the Node harness (scripts/sandbox-node.ts).

import type { BuildOptions, Plugin } from 'esbuild';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);

/**
 * Absolute path of a file inside one of this package's dependencies, e.g.
 * `chart.js/dist/chart.umd.min.js` (src/host/loader.ts LIBRARIES). The libraries'
 * `exports` maps do not expose their dist files, so this resolves the package
 * directory (the nearest package.json above the main entry) and joins the rest.
 */
export function libraryPath(source: string): string {
  const [name, ...rest] = source.startsWith('@') ? [source.split('/').slice(0, 2).join('/'), ...source.split('/').slice(2)] : [source.split('/')[0], ...source.split('/').slice(1)];
  let dir = path.dirname(require.resolve(name));
  while (!fs.existsSync(path.join(dir, 'package.json')) || JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')).name !== name) {
    const parent = path.dirname(dir);
    if (parent === dir) throw new Error(`package directory of ${name} not found`);
    dir = parent;
  }
  return path.join(dir, ...rest);
}

/**
 * linkedom's `import 'css-select'` → our wrapper; the wrapper's own
 * `import 'css-select'` → the real package (resolved from linkedom's
 * node_modules, so no extra dependency is needed).
 */
function cssSelectShim(root: string): Plugin {
  const shim = path.join(root, 'src/sandbox/css-select-shim.ts');
  const linkedomDir = path.dirname(require.resolve('linkedom/package.json', { paths: [root] }));
  const real = require.resolve('css-select', { paths: [linkedomDir] });
  return {
    name: 'css-select-shim',
    setup(build) {
      build.onResolve({ filter: /^css-select$/ }, (args) => ({ path: args.importer === shim ? real : shim }));
    },
  };
}

export function sandboxBuildOptions(root: string): BuildOptions {
  return {
    entryPoints: [path.join(root, 'src/sandbox/index.ts')],
    bundle: true,
    format: 'iife',
    write: false,
    minify: true,
    platform: 'neutral',
    mainFields: ['module', 'main'],
    target: 'es2022',
    logLevel: 'warning',
    alias: { canvas: path.join(root, 'src/sandbox/canvas-stub.cjs') },
    plugins: [cssSelectShim(root)],
  };
}

/** `transport`: 'port' for the web host (MessageChannel), 'webkit' for WKWebView hosts, 'android' for android.webkit.WebView hosts. */
export function rendererBuildOptions(root: string, transport: 'port' | 'webkit' | 'android' = 'port'): BuildOptions {
  return {
    entryPoints: [path.join(root, 'src/renderer/renderer.ts')],
    bundle: true,
    format: 'iife',
    write: false,
    minify: true,
    platform: 'neutral',
    target: 'es2022',
    logLevel: 'warning',
    define: { __TW_TRANSPORT__: JSON.stringify(transport) },
  };
}

/** The validator as an IIFE for a trusted QuickJS context on native hosts (see src/host/validator-bundle.ts). */
export function validatorBuildOptions(root: string): BuildOptions {
  return {
    entryPoints: [path.join(root, 'src/host/validator-bundle.ts')],
    bundle: true,
    format: 'iife',
    write: false,
    minify: true,
    // 'browser' picks css-tree's prebuilt data/version modules (its lib/ uses node's createRequire).
    platform: 'browser',
    target: 'es2022',
    logLevel: 'warning',
  };
}
