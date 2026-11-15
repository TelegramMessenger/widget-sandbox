import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

const pkgRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const root = path.resolve(pkgRoot, '..', '..');

// The Swift package ships the .js in WidgetSandbox and keeps the .qbc in WidgetSandboxQuickJS, which an app does not link.
const SHIPPED: Record<string, { js: string; bytecode?: string }> = {
  ios: { js: 'packages/ios/Sources/WidgetSandbox/WidgetJS', bytecode: 'packages/ios/Sources/WidgetSandboxQuickJS/WidgetJS' },
  android: { js: 'packages/android/widget-sandbox/src/main/assets/WidgetJS' },
  desktop: { js: 'packages/desktop/WidgetJS' },
};

function files(dir: string, prefix = ''): string[] {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? files(path.join(dir, e.name), path.join(prefix, e.name)) : [path.join(prefix, e.name)]).sort();
}

describe('shipped native bundles', () => {
  for (const [target, shipped] of Object.entries(SHIPPED)) {
    it(`${target} ships exactly what a fresh build produces (rebuild with pnpm build:${target})`, () => {
      const out = fs.mkdtempSync(path.join(os.tmpdir(), `tw-bundles-${target}-`));
      try {
        const bytecodeOut = path.join(out, 'bytecode');
        const jsOut = path.join(out, 'js');
        const args = [path.join(pkgRoot, 'scripts', 'build-native-bundles.ts'), target, '--out', jsOut];
        if (shipped.bytecode) args.push('--bytecode-out', bytecodeOut);
        execFileSync(process.execPath, ['--import', 'tsx', ...args], { cwd: pkgRoot, stdio: 'pipe' });
        const pairs: [string, string][] = [[shipped.js, jsOut]];
        if (shipped.bytecode) pairs.push([shipped.bytecode, bytecodeOut]);
        for (const [dir, freshDir] of pairs) {
          const fresh = files(freshDir);
          const actual = files(path.join(root, dir));
          assert.deepEqual(actual, fresh, `${dir}: file list differs from a fresh build`);
          const stale = fresh.filter((f) => !fs.readFileSync(path.join(freshDir, f)).equals(fs.readFileSync(path.join(root, dir, f))));
          assert.deepEqual(stale, [], `${dir}: stale against a fresh build`);
        }
      } finally {
        fs.rmSync(out, { recursive: true, force: true });
      }
    });
  }
});
