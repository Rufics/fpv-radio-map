// Playwright screenshots of the mock build. Build first: npm run build:mock
// Chromium comes from PLAYWRIGHT_BROWSERS_PATH (default /opt/pw-browsers); no `playwright install`.
import { chromium } from 'playwright-core';
import { existsSync, mkdirSync } from 'node:fs';
import { serve } from './serve-dist.mjs';

const DIST = process.env.DIST ?? 'dist/fpv-app-mock/browser';
const OUT = 'screenshots';
const PORT = 4300;
const BASE = `http://127.0.0.1:${PORT}`;
const browsersPath = process.env.PLAYWRIGHT_BROWSERS_PATH ?? '/opt/pw-browsers';
const exe = [`${browsersPath}/chromium-1194/chrome-linux/chrome`, `${browsersPath}/chromium`].find((p) => existsSync(p));

mkdirSync(OUT, { recursive: true });
const server = await serve(DIST, PORT);
const browser = await chromium.launch({ executablePath: exe, args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const errors = [];
const watch = (page, tag) => {
  page.on('pageerror', (e) => errors.push(`[${tag}] ${e.message}`));
  page.on('console', (m) => m.type() === 'error' && errors.push(`[${tag}] console: ${m.text()}`));
};
const settle = (page, ms = 1500) => page.waitForTimeout(ms);

try {
  // ---- laptop: result state + why drawer ----
  const laptop = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  watch(laptop, 'laptop');
  await laptop.goto(`${BASE}/?scenario=normal`);
  await laptop.getByRole('button', { name: 'Use example route' }).click();
  await laptop.getByRole('button', { name: /^Calculate$/ }).click();
  await laptop.getByRole('button', { name: 'Show why' }).waitFor({ timeout: 20000 });
  await laptop.getByText('Recalculate').or(laptop.getByText('Calculated')).first().waitFor({ timeout: 20000 }).catch(() => {});
  await settle(laptop, 4500);
  await laptop.screenshot({ path: `${OUT}/laptop-result.png` });
  await laptop.getByRole('button', { name: 'Show why' }).click();
  await settle(laptop, 5000);
  await laptop.screenshot({ path: `${OUT}/laptop-why.png` });
  // extra: fix preview (before/after) and the share dialog
  await laptop.getByRole('button', { name: /^Preview fix: / }).click();
  await settle(laptop, 2500);
  await laptop.screenshot({ path: `${OUT}/laptop-fix-preview.png` });
  await laptop.getByRole('button', { name: 'Cancel', exact: true }).click();
  await laptop.getByRole('button', { name: 'Share', exact: true }).click();
  await settle(laptop, 1500);
  await laptop.screenshot({ path: `${OUT}/laptop-share.png` });
  // extra: compare scenario B (feature 13): other video ground antenna, then the coverage difference map
  await laptop.getByRole('button', { name: 'Close share dialog' }).click();
  await laptop.getByRole('button', { name: 'Close why drawer' }).click();
  await laptop.getByLabel('What changes in B').selectOption('video-antenna');
  await laptop.getByLabel('Ground antenna in B').selectOption('patch-8dbi-915');
  await laptop.getByRole('button', { name: 'Compare A and B' }).click();
  await laptop.getByRole('button', { name: 'Coverage difference map' }).waitFor({ timeout: 20000 });
  await laptop.getByText('Recalculate').first().waitFor({ timeout: 20000 }).catch(() => {});
  await settle(laptop, 4000);
  await laptop.getByRole('button', { name: 'Coverage difference map' }).click();
  await laptop.getByRole('button', { name: 'Hide' }).waitFor({ timeout: 20000 });
  await settle(laptop, 2500);
  await laptop.locator('fpv-compare-panel').scrollIntoViewIfNeeded();
  await laptop.screenshot({ path: `${OUT}/laptop-compare.png` });
  // extra: empty state and a module failure state
  const empty = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  watch(empty, 'empty');
  await empty.goto(`${BASE}/?scenario=module-failed`);
  await settle(empty, 2500);
  await empty.screenshot({ path: `${OUT}/laptop-empty.png` });
  await empty.getByRole('button', { name: 'Use example route' }).click();
  await empty.getByRole('button', { name: /^Calculate$/ }).click();
  await settle(empty, 7000);
  await empty.screenshot({ path: `${OUT}/laptop-module-failed.png` });
  await empty.goto(`${BASE}/admin/modules`);
  await settle(empty, 1500);
  await empty.screenshot({ path: `${OUT}/admin-modules.png` });
  await empty.goto(`${BASE}/admin/library`);
  await empty.getByRole('button', { name: 'Batteries' }).click();
  await empty.getByRole('button', { name: /6S LiPo 3300/ }).click();
  await settle(empty, 800);
  await empty.screenshot({ path: `${OUT}/admin-library.png` });

  // ---- phone: PIN entry, then the mission view ----
  const phone = await browser.newPage({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
  watch(phone, 'phone');
  await phone.goto(`${BASE}/m/v/demo-view`);
  for (const d of '24') await phone.getByRole('button', { name: `Digit ${d}` }).click();
  await settle(phone, 300);
  await phone.screenshot({ path: `${OUT}/phone-pin.png` });
  for (const d of '6810') await phone.getByRole('button', { name: `Digit ${d}` }).click();
  await phone.getByRole('button', { name: 'Open mission' }).click();
  await phone.waitForURL(/\/mission\//, { timeout: 10000 });
  await settle(phone, 3500);
  await phone.screenshot({ path: `${OUT}/phone-mission.png` });
  const failing = phone.locator('button.leg.bad').first();
  if (await failing.count()) {
    await failing.click();
    await settle(phone, 1500);
    await phone.screenshot({ path: `${OUT}/phone-why.png` });
  }
} finally {
  await browser.close();
  server.close();
}
if (errors.length) console.log('Page errors:\n' + errors.join('\n'));
console.log(`Screenshots written to ${OUT}/`);
