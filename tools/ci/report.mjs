// Builds the Markdown test report the CI posts to a pull request and to the job summary.
//   node report.mjs RESULTS_DIR JOBS_JSON > report.md
// RESULTS_DIR holds JUnit XML files (ctest --output-junit, vitest junit reporter).
// JOBS_JSON is the `needs` context of the report job: {"cpp": {"result": "success"}, ...}.
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, basename } from 'node:path';

export const MARKER = '<!-- rfmap-ci-report -->';

const LABELS = {
  'cpp.xml': 'C++: rf-core, rf-ext, rfmod, FPV modules, server (with PostGIS)',
  'web-ui-kit.xml': 'Web: ui-kit',
  'web-fpv-app.xml': 'Web: fpv-app',
};

const attr = (tag, name) => {
  const m = tag.match(new RegExp(`\\s${name}="([^"]*)"`));
  return m ? Number(m[1]) : 0;
};

const unescape = (s) => s.replaceAll('&quot;', '"').replaceAll('&apos;', "'").replaceAll('&lt;', '<').replaceAll('&gt;', '>').replaceAll('&amp;', '&');

/** Totals and failed test names of one JUnit file. */
export function parseJunit(xml) {
  const cases = [...xml.matchAll(/<testcase\b([^>]*?)(\/>|>([\s\S]*?)<\/testcase>)/g)];
  const failed = [];
  let skipped = 0;
  for (const c of cases) {
    const body = c[3] ?? '';
    const name = unescape((c[1].match(/\sname="([^"]*)"/) ?? [])[1] ?? '?');
    const cls = unescape((c[1].match(/\sclassname="([^"]*)"/) ?? [])[1] ?? '');
    if (/<(failure|error)\b/.test(body) || /\sstatus="fail(ed)?"/.test(c[1])) failed.push(cls && cls !== name ? `${cls} › ${name}` : name);
    else if (/<skipped\b/.test(body) || /\sstatus="(notrun|disabled|skipped)"/.test(c[1])) skipped++;
  }
  // Prefer the counts the runner wrote on the root element; fall back to counting cases.
  const root = (xml.match(/<testsuites?\b[^>]*>/) ?? [''])[0];
  const total = attr(root, 'tests') || cases.length;
  return { total, failed, skipped, passed: total - failed.length - skipped };
}

export function buildReport(dir, allJobs, context = {}) {
  // "changes" only decides which jobs run; it is not a result worth a row.
  const jobs = Object.fromEntries(Object.entries(allJobs).filter(([name]) => name !== 'changes'));
  const lines = [MARKER, '## Test report', ''];
  const files = (() => {
    try {
      return readdirSync(dir).filter((f) => f.endsWith('.xml') && statSync(join(dir, f)).isFile()).sort();
    } catch {
      return [];
    }
  })();
  const failures = [];
  if (files.length) {
    lines.push('| Suite | Passed | Failed | Skipped |', '|---|---:|---:|---:|');
    for (const f of files) {
      const r = parseJunit(readFileSync(join(dir, f), 'utf8'));
      lines.push(`| ${LABELS[f] ?? basename(f, '.xml')} | ${r.passed} | ${r.failed.length ? `**${r.failed.length}**` : 0} | ${r.skipped} |`);
      failures.push(...r.failed.map((n) => `${LABELS[f] ?? f}: ${n}`));
    }
    lines.push('');
  } else if (Object.values(jobs).length && Object.values(jobs).every((v) => v?.result === 'skipped')) {
    lines.push('Nothing to build: this change touches only files the tests do not cover, such as docs.', '');
  } else {
    lines.push('No test results were uploaded; see the jobs below.', '');
  }
  if (failures.length) {
    lines.push('<details open><summary>Failed tests</summary>', '');
    for (const f of failures.slice(0, 50)) lines.push(`- ${f}`);
    if (failures.length > 50) lines.push(`- … and ${failures.length - 50} more`);
    lines.push('', '</details>', '');
  }
  lines.push('| Job | Result |', '|---|---|');
  for (const [name, v] of Object.entries(jobs)) {
    const r = v?.result ?? 'unknown';
    const icon = r === 'success' ? '✅' : r === 'skipped' ? '⏭️' : '❌';
    lines.push(`| ${name} | ${icon} ${r} |`);
  }
  if (context.sha) lines.push('', `Commit ${context.sha.slice(0, 7)}${context.runUrl ? ` · [workflow run](${context.runUrl})` : ''}`);
  return lines.join('\n') + '\n';
}

if (process.argv[1] && import.meta.url.endsWith(basename(process.argv[1]))) {
  const [dir = 'test-results', jobsJson = '{}'] = process.argv.slice(2);
  process.stdout.write(
    buildReport(dir, JSON.parse(jobsJson), { sha: process.env.REPORT_SHA, runUrl: process.env.REPORT_RUN_URL }),
  );
}
