// Turns runner output into the common conformance report and a Markdown
// summary. Input: one runner file per client renderer in the run format
// (reports/conformance/<renderer>.json from run-web.ts; native runners write
// the same shape, see corpus/conformance/README.md). Output:
//   corpus/conformance/results/<platform>-<renderer>.json   the common report
//   corpus/conformance/results/summary.md                    all renderers side by side
// The native Chromium/WebKit answers recorded in manifest.json by build.ts are
// added as reference columns ("native-chromium"): what a plain browser engine
// says, without the sandbox, the validator or the profile.
//
//   corepack pnpm tsx scripts/conformance/report.ts [reports/conformance/*.json …]

import { execSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { CATALOGUE, type Feature } from './catalogue';
import { CONF, ROOT, readJson } from './lib';

type St = 'pass' | 'fail' | 'xfail' | 'xpass' | 'skip';
export type Verdict = 'pass' | 'partial' | 'fail' | 'xfail' | 'xpass' | 'skip' | 'untested';

interface RunFile {
  renderer: string; platform?: string; engine?: string; features?: string[] | string; policy?: string; date?: string;
  runs: {
    id: string; set: string; theme: string; width: number; loaded: boolean; dead: string | null; status: string;
    checks: [string, string, string, string][]; stages: { name: string; covers: string[]; diff: number | null; status: St; maxDiff: number; detail?: string }[];
    host: { id: string; name: string; status: string; detail: string }[]; errors: string[]; skippedActions?: string[];
  }[];
}

/** One result for one catalogue ID: where it comes from and how it went. */
interface Evidence { test: string; kind: 'check' | 'stage' | 'host'; name: string; status: St; detail: string; theme: string; width: number }

const norm = (s: string): St => (s === 'pending' ? 'fail' : (s as St));

export function verdictOf(ev: { status: St }[]): Verdict {
  if (!ev.length) return 'untested';
  const n = (s: St) => ev.filter((e) => e.status === s).length;
  const pass = n('pass'), fail = n('fail'), xfail = n('xfail'), xpass = n('xpass');
  if (fail && (pass || xpass)) return 'partial';
  if (fail) return 'fail';
  if (xpass && !xfail && !pass) return 'xpass';
  // What is implemented passes, the rest is known to be pending: not a failure.
  if (xfail) return 'xfail';
  if (pass || xpass) return 'pass';
  return 'skip';
}

export function buildReport(file: RunFile) {
  const evidence = new Map<string, Evidence[]>();
  const add = (id: string, e: Evidence) => { if (!evidence.has(id)) evidence.set(id, []); evidence.get(id)!.push(e); };
  const tests = file.runs.map((r) => {
    const failed = !r.loaded || !!r.dead;
    if (r.set === 'auto') {
      for (const [id, name, st, detail] of r.checks) if (id !== 'harness') add(id, { test: r.id, kind: 'check', name, status: failed ? 'fail' : norm(st), detail, theme: r.theme, width: r.width });
      for (const s of r.stages) for (const id of s.covers) add(id, { test: r.id, kind: 'stage', name: s.name, status: failed ? 'fail' : s.status, detail: s.diff === null ? 'no reference' : `diff ${(s.diff * 100).toFixed(1)} % of ink (max ${(s.maxDiff * 100).toFixed(0)} %)${s.detail ? '; ' + s.detail : ''}`, theme: r.theme, width: r.width });
      for (const h of r.host) add(h.id, { test: r.id, kind: 'host', name: h.name, status: norm(h.status), detail: h.detail, theme: r.theme, width: r.width });
    }
    const harness = r.checks.filter((c) => c[0] === 'harness');
    return {
      id: r.id, set: r.set, theme: r.theme, width: r.width, loaded: r.loaded, dead: r.dead, status: r.status,
      errors: r.errors, harnessErrors: harness.map((c) => c[1] + ': ' + c[3]), skippedActions: r.skippedActions ?? [],
      counts: r.checks.reduce((m: Record<string, number>, c) => ({ ...m, [norm(c[2])]: (m[norm(c[2])] ?? 0) + 1 }), {}),
      stages: r.stages.map((s) => ({ name: s.name, diff: s.diff, status: s.status })),
      host: r.host,
    };
  });
  const features: Record<string, { status: Feature['status']; verdict: Verdict; counts: Record<string, number>; evidence: Evidence[] }> = {};
  for (const f of CATALOGUE) {
    const ev = evidence.get(f.id) ?? [];
    features[f.id] = { status: f.status, verdict: verdictOf(ev), counts: ev.reduce((m: Record<string, number>, e) => ({ ...m, [e.status]: (m[e.status] ?? 0) + 1 }), {}), evidence: ev };
  }
  const summary: Record<string, Record<Verdict, number>> = {};
  for (const st of ['in', 'candidate', 'out'] as const) {
    const row = { pass: 0, partial: 0, fail: 0, xfail: 0, xpass: 0, skip: 0, untested: 0 } as Record<Verdict, number>;
    for (const f of CATALOGUE.filter((x) => x.status === st)) row[features[f.id].verdict]++;
    summary[st] = row;
  }
  let commit = '';
  try { commit = execSync('git rev-parse --short HEAD', { cwd: ROOT }).toString().trim(); } catch {}
  return {
    schema: 1,
    client: { platform: file.platform ?? 'web', renderer: file.renderer, engine: file.engine ?? '', policy: file.policy ?? 'strict', features: Array.isArray(file.features) ? file.features : String(file.features ?? '').split(',').filter(Boolean), date: file.date ?? new Date().toISOString(), commit },
    summary,
    features,
    tests,
  };
}

/** The native engine answers from manifest.json as a report-like column (checks only: stages are the reference itself). */
function nativeColumn(engine: string) {
  const m = readJson<{ tests: { id: string; set: string; checks: { id: string; name: string; native: Record<string, string> }[] }[] }>(path.join(CONF, 'manifest.json'), { tests: [] });
  const ev = new Map<string, { status: St }[]>();
  for (const t of m.tests) if (t.set === 'auto') for (const c of t.checks) {
    const st = c.native[engine];
    if (!st || c.id === 'harness') continue;
    if (!ev.has(c.id)) ev.set(c.id, []);
    ev.get(c.id)!.push({ status: norm(st) });
  }
  return Object.fromEntries(CATALOGUE.map((f) => [f.id, verdictOf(ev.get(f.id) ?? [])]));
}

const MARK: Record<Verdict, string> = { pass: '✅', partial: '◐', fail: '❌', xfail: '⏳', xpass: '❗', skip: '–', untested: '·' };

function main() {
  const args = process.argv.slice(2);
  const inputs = args.length ? args : fs.readdirSync(path.join(ROOT, 'reports', 'conformance')).filter((f) => f.endsWith('.json')).map((f) => path.join(ROOT, 'reports', 'conformance', f));
  const order = ['iframe', 'shadow', 'blitz', 'litehtml'];
  const rank = (r: string) => (order.includes(r) ? order.indexOf(r) : order.length);
  const reports = inputs.map((f) => buildReport(JSON.parse(fs.readFileSync(f, 'utf8')) as RunFile))
    .sort((a, b) => rank(a.client.renderer) - rank(b.client.renderer));
  const outDir = path.join(CONF, 'results');
  fs.mkdirSync(outDir, { recursive: true });
  for (const r of reports) {
    const file = path.join(outDir, `${r.client.platform}-${r.client.renderer}.json`);
    fs.writeFileSync(file, JSON.stringify(r, null, 1) + '\n');
    console.log(`${path.relative(ROOT, file)}: in ${JSON.stringify(r.summary.in)}`);
  }
  const natives = ['chromium', 'webkit'].map((e) => ({ name: `native ${e}`, verdicts: nativeColumn(e) })).filter((n) => Object.values(n.verdicts).some((v) => v !== 'untested'));
  const cols = [...reports.map((r) => ({ name: `${r.client.platform} ${r.client.renderer}`, verdicts: Object.fromEntries(Object.entries(r.features).map(([k, v]) => [k, v.verdict])) as Record<string, Verdict> })), ...natives];

  const md: string[] = [
    '# Conformance results',
    '',
    `Generated ${new Date().toISOString().slice(0, 16).replace('T', ' ')} UTC by \`scripts/conformance/report.ts\` from the runs of \`scripts/conformance/run-web.ts\` (commit ${reports[0]?.client.commit ?? '?'}, ${reports[0]?.client.engine ?? ''}, policy ${reports[0]?.client.policy ?? ''}, features ${(reports[0]?.client.features ?? []).join(', ') || 'none'}). Per-renderer reports in the common format: ${reports.map((r) => `[${r.client.platform}-${r.client.renderer}.json](${r.client.platform}-${r.client.renderer}.json)`).join(', ')}.`,
    '',
    `Verdict per catalogue ID over the automatic set: ${MARK.pass} pass, ${MARK.partial} partly, ${MARK.fail} fail, ${MARK.xfail} expected to fail (not implemented yet: D23, the profile in the validator, …), ${MARK.xpass} passes although expected to fail, ${MARK.skip} not applicable here, ${MARK.untested} not tested. For **out** features "pass" means *neutralised as the profile says*. The native columns are a plain browser engine without the pipeline: a reference for the tests, not an expectation.`,
    '',
    '## Totals',
    '',
    `| Renderer | in (${CATALOGUE.filter((f) => f.status === 'in').length}) ${MARK.pass} / ${MARK.partial} / ${MARK.fail} / ${MARK.xfail} | candidate (${CATALOGUE.filter((f) => f.status === 'candidate').length}) ${MARK.pass} / ${MARK.partial} / ${MARK.fail} | out (${CATALOGUE.filter((f) => f.status === 'out').length}) neutralised ${MARK.pass} / not yet ${MARK.xfail} / ${MARK.fail} | auto runs loaded | errors |`,
    '|---|---|---|---|---|---|',
  ];
  for (const r of reports) {
    const s = r.summary;
    const auto = r.tests.filter((t) => t.set === 'auto');
    md.push(`| ${r.client.platform} ${r.client.renderer} | ${s.in.pass} / ${s.in.partial} / ${s.in.fail} / ${s.in.xfail} | ${s.candidate.pass} / ${s.candidate.partial} / ${s.candidate.fail} | ${s.out.pass} / ${s.out.xfail + s.out.partial} / ${s.out.fail} | ${auto.filter((t) => t.loaded && !t.dead).length} / ${auto.length} | ${r.tests.reduce((n, t) => n + t.errors.length + t.harnessErrors.length, 0)} |`);
  }
  md.push('');
  const candidateRows = CATALOGUE.filter((f) => f.status === 'candidate');
  md.push('## Candidates', '', 'Membership decided by measurement (D3: in when every client draws it).', '', `| ID | Feature | ${cols.map((c) => c.name).join(' | ')} |`, `|---|---|${cols.map(() => '---').join('|')}|`);
  for (const f of candidateRows) md.push(`| \`${f.id}\` | ${f.title.replace(/\|/g, '\\|')} | ${cols.map((c) => MARK[c.verdicts[f.id]]).join(' | ')} |`);
  md.push('');
  for (const st of ['in', 'out'] as const) {
    md.push(`## ${st === 'in' ? 'In the profile' : 'Out of the profile (pass = neutralised)'}`, '', `| ID | ${cols.map((c) => c.name).join(' | ')} |`, `|---|${cols.map(() => '---').join('|')}|`);
    for (const f of CATALOGUE.filter((x) => x.status === st)) md.push(`| \`${f.id}\`${f.conditional ? ' *' : ''} | ${cols.map((c) => MARK[c.verdicts[f.id]]).join(' | ')} |`);
    md.push('');
  }
  md.push('`*` in the profile if every client draws it (D3a).', '');
  // Failures in detail for the product renderers: what to look at first.
  for (const r of reports.filter((x) => x.client.renderer === 'shadow' || x.client.renderer === 'iframe')) {
    md.push(`## ${r.client.platform} ${r.client.renderer}: in-profile failures`, '');
    let any = false;
    for (const f of CATALOGUE.filter((x) => x.status === 'in')) {
      const v = r.features[f.id];
      if (v.verdict !== 'fail' && v.verdict !== 'partial') continue;
      any = true;
      const bad = v.evidence.filter((e) => e.status === 'fail').slice(0, 4).map((e) => `${e.test.split('/')[1]}: ${e.name}${e.detail ? ` (${e.detail.slice(0, 90)})` : ''}`);
      md.push(`- \`${f.id}\`: ${bad.join('; ').replace(/\|/g, '\\|')}`);
    }
    if (!any) md.push('- none');
    md.push('');
  }
  md.push('## Visual set', '', 'For people (the test channel): judged by eye. The runner only checks that each widget loads without errors and records its few self-checks.', '', `| Widget | ${reports.map((r) => r.client.renderer).join(' | ')} |`, `|---|${reports.map(() => '---').join('|')}|`);
  const visualIds = [...new Set(reports.flatMap((r) => r.tests.filter((t) => t.set === 'visual').map((t) => t.id)))].sort();
  for (const id of visualIds) md.push(`| ${id} | ${reports.map((r) => { const t = r.tests.find((x) => x.id === id && x.theme === 'light') ?? r.tests.find((x) => x.id === id); return !t ? '·' : !t.loaded || t.dead ? '❌ ' + (t.dead ?? 'not loaded').slice(0, 40) : t.errors.length ? `⚠️ ${t.errors.length} errors` : '✅'; }).join(' | ')} |`);
  md.push('');
  fs.writeFileSync(path.join(outDir, 'summary.md'), md.join('\n'));
  console.log(`${path.relative(ROOT, path.join(outDir, 'summary.md'))}`);
}

if (import.meta.url === `file://${process.argv[1]}`) main();
