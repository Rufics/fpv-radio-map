// Builds the Markdown CI report that is posted to a pull request and to the job summary.
//
//   node report.mjs --jobs JOBS|--jobs-file FILE [--results DIR] [--coverage FILE] [--diff FILE]
//                   [--sha SHA] [--repo OWNER/NAME] [--run-url URL] [--coverage-url URL]
//
// --jobs      JSON: either the `needs` context of a job ({"cpp": {"result": "success"}, ...}) or
//             an array of {name, result, url} as listed by the Actions API.
// --jobs-file the same JSON, read from a file.
// --results   folder of JUnit XML files (ctest --output-junit, vitest junit reporter).
// --coverage  gcovr JSON report (gcovr --json) of the instrumented C++ build.
// --diff      `git diff --unified=0` of the pull request, for the coverage of the changed lines.
//
// Every input may come from a pull request of a fork, so it is treated as untrusted data: sizes
// are capped, nothing is executed, and every string that reaches the Markdown is escaped.
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, basename } from 'node:path';
import { parseArgs } from 'node:util';

export const MARKER = '<!-- rfmap-ci-report -->';

const MAX_INPUT_BYTES = 64 * 1024 * 1024;
const MAX_LISTED = 20;

/** Display names of the CI jobs, keyed by their id in ci.yml. */
export const JOB_NAMES = {
  cpp: 'C++ build and tests',
  coverage: 'C++ coverage',
  web: 'Web build and tests',
};

/** Jobs that only orchestrate the run; they are not results worth a row. */
const HIDDEN_JOBS = new Set(['changes', 'report', 'Changed parts', 'Test report']);

const SUITES = {
  'cpp.xml': 'C++',
  'web-ui-kit.xml': 'Web: ui-kit',
  'web-fpv-app.xml': 'Web: fpv-app',
};

/**
 * Coverage areas, most specific first. `area` groups the summary table; `component` is the row
 * in the per-component table. Files that match nothing are counted in "All C++" only.
 */
const COMPONENTS = [
  { prefix: 'core/rf-core/', area: 'Core', component: 'rf-core' },
  { prefix: 'core/rf-ext/', area: 'Core', component: 'rf-ext' },
  { prefix: 'platform/rfmod/', area: 'Platform', component: 'rfmod' },
  { prefix: 'modules/fpv/common/', area: 'FPV modules', component: 'fpv common' },
  { prefix: 'modules/fpv/', area: 'FPV modules', component: (rest) => `fpv.${rest.split('/')[0]}` },
  { prefix: 'server/', area: 'Server', component: 'server' },
];
const AREAS = [
  { name: 'Core', label: 'Core: rf-core, rf-ext' },
  { name: 'FPV modules', label: 'FPV modules' },
  { name: 'Platform', label: 'Platform: rfmod' },
  { name: 'Server', label: 'Server' },
];

// ---------------------------------------------------------------------------------------------
// Escaping and small helpers

/** Plain text for a Markdown table cell or list item: no HTML, links, mentions or table breaks. */
export function md(text, max = 200) {
  let s = String(text ?? '')
    .replace(/[\u0000-\u001f\u007f]+/g, ' ')
    .trim();
  if (s.length > max) s = `${s.slice(0, max - 1)}…`;
  return s
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replace(/[\\`*_{}[\]()|~]/g, (c) => `\\${c}`)
    .replace(/@/g, '@\u200b')
    .replace(/:\/\//g, ':\u200b//');
}

/** Repository paths that are safe to show and to put in a URL. */
const SAFE_PATH = /^[A-Za-z0-9_][A-Za-z0-9_.+/-]{0,300}$/;
export const safePath = (p) => typeof p === 'string' && SAFE_PATH.test(p) && !p.split('/').includes('..');

const SAFE_SHA = /^[0-9a-f]{7,64}$/;
const SAFE_REPO = /^[A-Za-z0-9_.-]{1,100}\/[A-Za-z0-9_.-]{1,100}$/;
const SAFE_URL = /^https:\/\/github\.com\/[A-Za-z0-9_./?=&%#-]{1,500}$/;
const safeUrl = (u) => (typeof u === 'string' && SAFE_URL.test(u) ? u : undefined);

function readCapped(file) {
  if (!file) return undefined;
  try {
    const st = statSync(file);
    if (!st.isFile() || st.size > MAX_INPUT_BYTES) return undefined;
    return readFileSync(file, 'utf8');
  } catch {
    return undefined;
  }
}

const pct = (covered, total) => (total ? (100 * covered) / total : undefined);
const fmtPct = (p) => (p === undefined ? '–' : `${p.toFixed(1)}%`);
const dot = (p) => (p === undefined ? '⚪' : p >= 80 ? '🟢' : p >= 60 ? '🟡' : '🔴');
const plural = (n, one, many = `${one}s`) => `${n} ${n === 1 ? one : many}`;

// ---------------------------------------------------------------------------------------------
// Tests (JUnit)

const attr = (tag, name) => {
  const m = tag.match(new RegExp(`\\s${name}="([^"]*)"`));
  return m ? Number(m[1]) || 0 : 0;
};

const unescapeXml = (s) =>
  s.replaceAll('&quot;', '"').replaceAll('&apos;', "'").replaceAll('&lt;', '<').replaceAll('&gt;', '>').replaceAll('&amp;', '&');

/** Totals and failed test names of one JUnit file. */
export function parseJunit(xml) {
  const cases = [...xml.matchAll(/<testcase\b([^>]*?)(\/>|>([\s\S]*?)<\/testcase>)/g)];
  const failed = [];
  let skipped = 0;
  for (const c of cases) {
    const body = c[3] ?? '';
    const name = unescapeXml((c[1].match(/\sname="([^"]*)"/) ?? [])[1] ?? '?');
    const cls = unescapeXml((c[1].match(/\sclassname="([^"]*)"/) ?? [])[1] ?? '');
    if (/<(failure|error)\b/.test(body) || /\sstatus="fail(ed)?"/.test(c[1])) failed.push(cls && cls !== name ? `${cls} › ${name}` : name);
    else if (/<skipped\b/.test(body) || /\sstatus="(notrun|disabled|skipped)"/.test(c[1])) skipped++;
  }
  // Prefer the counts the runner wrote on the root element; fall back to counting cases.
  const root = (xml.match(/<testsuites?\b[^>]*>/) ?? [''])[0];
  const total = Math.max(attr(root, 'tests') || cases.length, failed.length + skipped);
  return { total, failed, skipped, passed: total - failed.length - skipped };
}

function readSuites(dir) {
  let files = [];
  try {
    files = readdirSync(dir).filter((f) => f.endsWith('.xml') && statSync(join(dir, f)).isFile()).sort();
  } catch {
    return [];
  }
  return files.flatMap((f) => {
    const xml = readCapped(join(dir, f));
    return xml === undefined ? [] : [{ file: f, label: SUITES[f] ?? basename(f, '.xml'), ...parseJunit(xml) }];
  });
}

// ---------------------------------------------------------------------------------------------
// Coverage (gcovr JSON)

/** Component of a repository path, or undefined for files outside the known areas. */
export function componentOf(path) {
  for (const c of COMPONENTS) {
    if (!path.startsWith(c.prefix)) continue;
    const name = typeof c.component === 'function' ? c.component(path.slice(c.prefix.length)) : c.component;
    return { area: c.area, component: name };
  }
  return undefined;
}

const emptyTotals = () => ({ lines: 0, linesCovered: 0, functions: 0, functionsCovered: 0 });
const add = (t, f) => {
  t.lines += f.lines;
  t.linesCovered += f.linesCovered;
  t.functions += f.functions;
  t.functionsCovered += f.functionsCovered;
};

/**
 * Per-file line and function coverage from a gcovr JSON report. Returns
 * Map<path, {lines, linesCovered, functions, functionsCovered, hits: Map<line, count>}>.
 */
export function parseCoverage(json) {
  const files = new Map();
  let data;
  try {
    data = JSON.parse(json);
  } catch {
    return files;
  }
  for (const f of Array.isArray(data?.files) ? data.files : []) {
    const path = typeof f?.file === 'string' ? f.file.replace(/^\.\//, '') : '';
    if (!safePath(path)) continue;
    const entry = files.get(path) ?? { ...emptyTotals(), hits: new Map(), fns: new Map() };
    for (const l of Array.isArray(f.lines) ? f.lines : []) {
      const n = Number(l?.line_number);
      if (!Number.isInteger(n) || n < 1 || l['gcovr/noncode'] || l['gcovr/excluded']) continue;
      entry.hits.set(n, Math.max(entry.hits.get(n) ?? 0, Number(l.count) || 0));
    }
    for (const fn of Array.isArray(f.functions) ? f.functions : []) {
      const key = `${fn?.lineno}:${fn?.name}`;
      const count = Number(fn?.execution_count ?? fn?.count) || 0;
      if (fn?.['gcovr/excluded']) continue;
      entry.fns.set(key, Math.max(entry.fns.get(key) ?? 0, count));
    }
    files.set(path, entry);
  }
  for (const e of files.values()) {
    e.lines = e.hits.size;
    e.linesCovered = [...e.hits.values()].filter((c) => c > 0).length;
    e.functions = e.fns.size;
    e.functionsCovered = [...e.fns.values()].filter((c) => c > 0).length;
    delete e.fns;
  }
  return files;
}

export function summarizeCoverage(files) {
  const total = emptyTotals();
  const areas = new Map();
  const components = new Map();
  for (const [path, f] of files) {
    add(total, f);
    const c = componentOf(path);
    if (!c) continue;
    if (!areas.has(c.area)) areas.set(c.area, emptyTotals());
    add(areas.get(c.area), f);
    const key = `${c.area}\u0000${c.component}`;
    if (!components.has(key)) components.set(key, { area: c.area, component: c.component, ...emptyTotals() });
    add(components.get(key), f);
  }
  return { total, areas, components: [...components.values()] };
}

// ---------------------------------------------------------------------------------------------
// Changed lines (git diff --unified=0)

/** Added or modified line numbers per file of a unified diff: Map<path, Set<line>>. */
export function parseDiff(diff) {
  const changed = new Map();
  let current;
  for (const line of diff.split('\n')) {
    if (line.startsWith('+++ ')) {
      const m = line.match(/^\+\+\+ b\/(.+)$/);
      current = m && safePath(m[1]) ? m[1] : undefined;
      if (current && !changed.has(current)) changed.set(current, new Set());
      continue;
    }
    const h = current && line.match(/^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@/);
    if (!h) continue;
    const start = Number(h[1]);
    const count = h[2] === undefined ? 1 : Number(h[2]);
    for (let n = start; n < start + Math.min(count, 100000); n++) changed.get(current).add(n);
  }
  return changed;
}

/** Coverage of the changed executable lines, with the uncovered ones as line ranges per file. */
export function changedLineCoverage(files, changed) {
  let total = 0;
  let covered = 0;
  const uncovered = [];
  for (const [path, lines] of changed) {
    const f = files.get(path);
    if (!f) continue;
    const missing = [];
    for (const n of [...lines].sort((a, b) => a - b)) {
      if (!f.hits.has(n)) continue;
      total++;
      if (f.hits.get(n) > 0) covered++;
      else missing.push(n);
    }
    for (const r of toRanges(missing)) uncovered.push({ path, ...r });
  }
  return { total, covered, uncovered };
}

function toRanges(sorted) {
  const out = [];
  for (const n of sorted) {
    const last = out.at(-1);
    if (last && n === last.to + 1) last.to = n;
    else out.push({ from: n, to: n });
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Jobs

const ICONS = { success: '✅', skipped: '⏭️', cancelled: '🚫', failure: '❌' };

export function normalizeJobs(input) {
  const list = Array.isArray(input)
    ? input.map((j) => ({ name: String(j?.name ?? '?'), result: String(j?.result ?? j?.conclusion ?? 'unknown'), url: safeUrl(j?.url) }))
    : Object.entries(input ?? {}).map(([id, v]) => ({ name: JOB_NAMES[id] ?? id, result: String(v?.result ?? 'unknown') }));
  return list.filter((j) => !HIDDEN_JOBS.has(j.name));
}

// ---------------------------------------------------------------------------------------------
// Report

export function buildReport({ jobs: rawJobs = {}, suites = [], coverage, diff, sha, repo, runUrl, coverageUrl } = {}) {
  const jobs = normalizeJobs(rawJobs);
  sha = SAFE_SHA.test(sha ?? '') ? sha : undefined;
  repo = SAFE_REPO.test(repo ?? '') ? repo : undefined;
  runUrl = safeUrl(runUrl);
  coverageUrl = safeUrl(coverageUrl);

  const failedTests = suites.flatMap((s) => s.failed.map((n) => ({ suite: s.label, name: n })));
  const failedJobs = jobs.filter((j) => j.result === 'failure' || j.result === 'cancelled');
  const passed = suites.reduce((n, s) => n + s.passed, 0);
  const ran = jobs.some((j) => j.result !== 'skipped') || suites.length > 0;
  const ok = failedJobs.length === 0 && failedTests.length === 0;

  const cov = coverage?.size ? summarizeCoverage(coverage) : undefined;
  const changed = cov && diff ? changedLineCoverage(coverage, diff) : undefined;

  const lines = [MARKER];

  // Headline: the one line a developer needs to read.
  const at = sha ? ` for ${sha.slice(0, 7)}` : '';
  if (!ok) {
    const what = failedTests.length
      ? plural(failedTests.length, 'test') + ' failed'
      : `${failedJobs.map((j) => j.name).join(', ')} failed`;
    lines.push(`### ❌ CI failed${at}: ${md(what, 300)}`);
  } else if (!ran) {
    lines.push(`### ✅ CI passed${at}`, '', 'Nothing to build: this change touches only files the tests do not cover, such as docs.');
  } else {
    lines.push(`### ✅ CI passed${at}`);
  }
  const facts = [];
  if (suites.length) facts.push(`**${passed}** tests passed`);
  if (cov) facts.push(`C++ line coverage **${fmtPct(pct(cov.total.linesCovered, cov.total.lines))}**`);
  if (changed?.total) facts.push(`changed lines **${fmtPct(pct(changed.covered, changed.total))}** covered`);
  if (facts.length) lines.push('', facts.join(' · '));
  lines.push('');

  if (failedTests.length) {
    lines.push('<details open><summary><b>Failed tests</b></summary>', '');
    for (const f of failedTests.slice(0, MAX_LISTED)) lines.push(`- ${md(f.suite, 40)}: ${md(f.name, 300)}`);
    if (failedTests.length > MAX_LISTED) lines.push(`- … and ${failedTests.length - MAX_LISTED} more`);
    lines.push('', '</details>', '');
  }

  // One row per job, with the test counts of the suites it produced.
  if (ran) {
    const suitesOf = (job) =>
      suites.filter((s) => (job.name.startsWith('C++ build') ? s.file === 'cpp.xml' : job.name.startsWith('Web') && s.file.startsWith('web-')));
    lines.push('| Check | Result | Details |', '|---|---|---|');
    for (const j of jobs) {
      const icon = ICONS[j.result] ?? '❔';
      let details = '';
      const own = suitesOf(j);
      if (own.length) {
        details = own
          .map((s) => {
            const counts = [`${s.passed} passed`];
            if (s.failed.length) counts.push(`**${s.failed.length} failed**`);
            if (s.skipped) counts.push(`${s.skipped} skipped`);
            return `${own.length > 1 || s.label !== 'C++' ? `${md(s.label, 40)}: ` : ''}${counts.join(', ')}`;
          })
          .join('<br>');
      } else if (j.name === JOB_NAMES.coverage && cov) {
        details = `${fmtPct(pct(cov.total.linesCovered, cov.total.lines))} of C++ lines`;
      } else if (j.result === 'failure') {
        details = 'failed before any test result was written; open the log';
      } else if (j.result === 'skipped') {
        details = 'not affected by this change';
      }
      const name = j.url ? `[${md(j.name, 80)}](${j.url})` : md(j.name, 80);
      lines.push(`| ${name} | ${icon} ${md(j.result, 20)} | ${details} |`);
    }
    lines.push('');
  }

  if (cov) {
    lines.push('#### C++ coverage', '', '| Area | Lines | Functions |', '|---|---:|---:|');
    const row = (label, t, strong = false) => {
      const lp = pct(t.linesCovered, t.lines);
      const fp = pct(t.functionsCovered, t.functions);
      const b = (s) => (strong ? `**${s}**` : s);
      return `| ${b(label)} | ${dot(lp)} ${b(fmtPct(lp))} <sub>${t.linesCovered}/${t.lines}</sub> | ${b(fmtPct(fp))} <sub>${t.functionsCovered}/${t.functions}</sub> |`;
    };
    for (const a of AREAS) if (cov.areas.has(a.name)) lines.push(row(a.label, cov.areas.get(a.name)));
    lines.push(row('All C++', cov.total, true), '');

    if (changed?.total) {
      const p = pct(changed.covered, changed.total);
      const missing = changed.total - changed.covered;
      lines.push(
        `**Changed lines:** ${dot(p)} ${fmtPct(p)} covered (${changed.covered} of ${plural(changed.total, 'executable line')} this pull request adds or changes).`,
      );
      if (missing) {
        lines.push('', `<details><summary>${plural(missing, 'changed line')} not run by any test</summary>`, '');
        const byFile = new Map();
        for (const u of changed.uncovered) byFile.set(u.path, [...(byFile.get(u.path) ?? []), u]);
        for (const [path, ranges] of [...byFile].slice(0, MAX_LISTED)) {
          const shown = ranges.slice(0, 10).map((u) => {
            const text = u.from === u.to ? `${u.from}` : `${u.from}-${u.to}`;
            const anchor = u.from === u.to ? `L${u.from}` : `L${u.from}-L${u.to}`;
            return repo && sha ? `[${text}](https://github.com/${repo}/blob/${sha}/${path}#${anchor})` : text;
          });
          if (ranges.length > 10) shown.push(`… ${ranges.length - 10} more`);
          // safePath() admits no backtick, so the path is shown as code without escaping.
          lines.push(`- \`${path}\`: ${shown.join(', ')}`);
        }
        if (byFile.size > MAX_LISTED) lines.push(`- … and ${plural(byFile.size - MAX_LISTED, 'more file')}`);
        lines.push('', '</details>');
      }
      lines.push('');
    } else if (diff) {
      lines.push('**Changed lines:** this pull request changes no executable C++ line.', '');
    }

    const comps = cov.components.sort(
      (a, b) => AREAS.findIndex((x) => x.name === a.area) - AREAS.findIndex((x) => x.name === b.area) || a.component.localeCompare(b.component),
    );
    if (comps.length) {
      lines.push('<details><summary>Coverage per component</summary>', '', '| Component | Lines | Functions |', '|---|---:|---:|');
      for (const c of comps) {
        const lp = pct(c.linesCovered, c.lines);
        lines.push(`| ${md(c.component, 60)} | ${dot(lp)} ${fmtPct(lp)} <sub>${c.linesCovered}/${c.lines}</sub> | ${fmtPct(pct(c.functionsCovered, c.functions))} |`);
      }
      lines.push('', '</details>', '');
    }
    lines.push('<sub>🟢 80% or more · 🟡 60–80% · 🔴 below 60%. Coverage is reported, not enforced. Test code is not counted.</sub>', '');
  }

  const links = [];
  if (runUrl) links.push(`[Workflow run](${runUrl})`);
  if (coverageUrl) links.push(`[HTML coverage report](${coverageUrl})`);
  if (links.length) lines.push(links.join(' · '));
  return lines.join('\n').replace(/\n{3,}/g, '\n\n').trimEnd() + '\n';
}

// ---------------------------------------------------------------------------------------------
// CLI

if (process.argv[1] && import.meta.url.endsWith(basename(process.argv[1]))) {
  const { values: o } = parseArgs({
    options: {
      jobs: { type: 'string', default: '{}' },
      'jobs-file': { type: 'string' },
      results: { type: 'string' },
      coverage: { type: 'string' },
      diff: { type: 'string' },
      sha: { type: 'string' },
      repo: { type: 'string' },
      'run-url': { type: 'string' },
      'coverage-url': { type: 'string' },
    },
  });
  const coverageJson = readCapped(o.coverage);
  const diffText = readCapped(o.diff);
  process.stdout.write(
    buildReport({
      jobs: JSON.parse(o['jobs-file'] ? (readCapped(o['jobs-file']) ?? '[]') : o.jobs),
      suites: o.results ? readSuites(o.results) : [],
      coverage: coverageJson === undefined ? undefined : parseCoverage(coverageJson),
      diff: diffText === undefined ? undefined : parseDiff(diffText),
      sha: o.sha,
      repo: o.repo,
      runUrl: o['run-url'],
      coverageUrl: o['coverage-url'],
    }),
  );
}
