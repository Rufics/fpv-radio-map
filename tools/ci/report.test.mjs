// Tests of the CI report builder: node --test tools/ci/report.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MARKER, buildReport, changedLineCoverage, componentOf, md, normalizeJobs, parseCoverage, parseDiff, parseJunit } from './report.mjs';

const gcovr = (files) => JSON.stringify({ 'gcovr/format_version': '0.6', files });
const file = (path, hits, fns = []) => ({
  file: path,
  lines: Object.entries(hits).map(([n, count]) => ({ line_number: Number(n), count, branches: [] })),
  functions: fns.map(([name, execution_count], i) => ({ name, lineno: i + 1, execution_count })),
});

test('JUnit totals and failed names', () => {
  const r = parseJunit(
    '<testsuites tests="3"><testcase classname="A" name="ok"/><testcase classname="A" name="bad"><failure/></testcase>' +
      '<testcase name="off"><skipped/></testcase></testsuites>',
  );
  assert.deepEqual(r, { total: 3, failed: ['A › bad'], skipped: 1, passed: 1 });
});

test('paths are grouped into core, modules, platform and server', () => {
  assert.deepEqual(componentOf('core/rf-core/src/a.cpp'), { area: 'Core', component: 'rf-core' });
  assert.deepEqual(componentOf('core/rf-ext/src/a.cpp'), { area: 'Core', component: 'rf-ext' });
  assert.deepEqual(componentOf('modules/fpv/relay/src/relay.cpp'), { area: 'FPV modules', component: 'fpv.relay' });
  assert.deepEqual(componentOf('modules/fpv/common/include/fpv/world.hpp'), { area: 'FPV modules', component: 'fpv common' });
  assert.deepEqual(componentOf('platform/rfmod/src/registry.cpp'), { area: 'Platform', component: 'rfmod' });
  assert.deepEqual(componentOf('server/src/app.cpp'), { area: 'Server', component: 'server' });
  assert.equal(componentOf('examples/modules/x.cpp'), undefined);
});

test('coverage counts each line once and ignores unsafe paths', () => {
  const files = parseCoverage(
    gcovr([
      file('core/rf-core/src/a.cpp', { 1: 1, 2: 0 }, [['f', 1], ['g', 0]]),
      { file: 'core/rf-core/src/a.cpp', lines: [{ line_number: 2, count: 3 }], functions: [] },
      file('../etc/passwd', { 1: 1 }),
      file('server/src/<b>.cpp', { 1: 1 }),
    ]),
  );
  assert.deepEqual([...files.keys()], ['core/rf-core/src/a.cpp']);
  const a = files.get('core/rf-core/src/a.cpp');
  assert.equal(a.lines, 2);
  assert.equal(a.linesCovered, 2);
  assert.equal(a.functions, 2);
  assert.equal(a.functionsCovered, 1);
  assert.equal(parseCoverage('not json').size, 0);
});

test('changed lines come from the new side of the diff', () => {
  const diff = [
    'diff --git a/core/rf-core/src/a.cpp b/core/rf-core/src/a.cpp',
    '--- a/core/rf-core/src/a.cpp',
    '+++ b/core/rf-core/src/a.cpp',
    '@@ -1,0 +2,3 @@',
    '+x',
    '@@ -9 +10 @@',
    '+y',
    'diff --git a/gone.cpp b/gone.cpp',
    '+++ /dev/null',
    '@@ -1,2 +0,0 @@',
  ].join('\n');
  const changed = parseDiff(diff);
  assert.deepEqual([...changed.keys()], ['core/rf-core/src/a.cpp']);
  assert.deepEqual([...changed.get('core/rf-core/src/a.cpp')], [2, 3, 4, 10]);

  const files = parseCoverage(gcovr([file('core/rf-core/src/a.cpp', { 2: 1, 3: 0, 4: 0, 10: 5, 11: 0 })]));
  const c = changedLineCoverage(files, changed);
  assert.equal(c.total, 4);
  assert.equal(c.covered, 2);
  assert.deepEqual(c.uncovered, [{ path: 'core/rf-core/src/a.cpp', from: 3, to: 4 }]);
});

test('job results from the needs context or from the Actions API', () => {
  assert.deepEqual(normalizeJobs({ changes: { result: 'success' }, cpp: { result: 'failure' } }), [
    { name: 'C++ build and tests', result: 'failure' },
  ]);
  assert.deepEqual(
    normalizeJobs([
      { name: 'Test report', conclusion: 'success' },
      { name: 'Web build and tests', conclusion: 'success', url: 'https://github.com/o/r/actions/runs/1/job/2' },
      { name: 'Evil', conclusion: 'success', url: 'javascript:alert(1)' },
    ]),
    [
      { name: 'Web build and tests', result: 'success', url: 'https://github.com/o/r/actions/runs/1/job/2' },
      { name: 'Evil', result: 'success', url: undefined },
    ],
  );
});

test('untrusted text cannot inject Markdown, HTML or mentions', () => {
  const s = md('<img src=x> [link](http://evil) @maintainer | `code`');
  assert.ok(!s.includes('<img'));
  assert.ok(!s.includes('](http'));
  assert.ok(!s.includes('@maintainer'));
  assert.ok(!/[^\\]\|/.test(s));
});

test('a passing run with coverage', () => {
  const coverage = parseCoverage(
    gcovr([file('core/rf-core/src/a.cpp', { 1: 1, 2: 1, 3: 1, 4: 0 }), file('modules/fpv/relay/src/r.cpp', { 1: 1, 2: 0 })]),
  );
  const out = buildReport({
    jobs: { changes: { result: 'success' }, cpp: { result: 'success' }, coverage: { result: 'success' }, web: { result: 'skipped' } },
    suites: [{ file: 'cpp.xml', label: 'C++', total: 5, passed: 5, skipped: 0, failed: [] }],
    coverage,
    diff: parseDiff('+++ b/core/rf-core/src/a.cpp\n@@ -0,0 +3,2 @@\n'),
    sha: '0123456789abcdef',
    commit: 'fedcba9876543210',
    repo: 'o/r',
  });
  assert.ok(out.startsWith(`${MARKER}\n### ✅ CI passed for 0123456`));
  assert.match(out, /\*\*5\*\* tests passed · C\+\+ line coverage \*\*66\.7%\*\* · changed lines \*\*50\.0%\*\* covered/);
  assert.match(out, /\| Core: rf-core, rf-ext \| 🟡 75\.0% <sub>3\/4<\/sub>/);
  assert.match(out, /\| FPV modules \| 🔴 50\.0% <sub>1\/2<\/sub>/);
  assert.match(out, /`core\/rf-core\/src\/a\.cpp`: \[4\]\(https:\/\/github\.com\/o\/r\/blob\/fedcba9876543210\/core\/rf-core\/src\/a\.cpp#L4\)/);
  assert.match(out, /\| Web build and tests \| ⏭️ skipped \| not affected by this change \|/);
});

test('a failing run leads with the failed tests', () => {
  const out = buildReport({
    jobs: { cpp: { result: 'failure' }, web: { result: 'success' } },
    suites: [{ file: 'cpp.xml', label: 'C++', total: 3, passed: 2, skipped: 0, failed: ['Geo › <b>Breaks</b>'] }],
  });
  assert.match(out, /^### ❌ CI failed: 1 test failed$/m);
  assert.match(out, /- C\+\+: Geo › &lt;b&gt;Breaks&lt;\/b&gt;/);
});

test('a build that fails before its tests says so', () => {
  const out = buildReport({ jobs: { cpp: { result: 'failure' } } });
  assert.match(out, /^### ❌ CI failed: C\+\+ build and tests failed$/m);
  assert.match(out, /failed before any test result was written/);
});

test('a docs-only change builds nothing', () => {
  const out = buildReport({ jobs: { changes: { result: 'success' }, cpp: { result: 'skipped' }, web: { result: 'skipped' } } });
  assert.match(out, /Nothing to build/);
  assert.ok(!out.includes('| Check |'));
});
