import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { ApiError, CredentialStore, Job, RfmapApiClient } from '@rfmap/ui-kit';
import { MockApiHarness } from '../tests/mock-api-harness';
import { LIBRARY, exampleMission } from './reference-case';

/** The dev mock backend, walked through with the app's own API client, keeps to the contract. */
describe('MockBackend through the API client', () => {
  let api: RfmapApiClient;
  let creds: CredentialStore;
  const settle = MockApiHarness.settle;

  async function rejection(p: Promise<unknown>): Promise<ApiError> {
    try {
      await settle(p);
    } catch (e) {
      return ApiError.from(e);
    }
    throw new Error('expected the request to fail');
  }

  beforeEach(() => {
    MockApiHarness.setUp();
    api = TestBed.inject(RfmapApiClient);
    creds = TestBed.inject(CredentialStore);
  });

  afterEach(() => {
    creds.setAdminToken(null);
    MockApiHarness.tearDown();
  });

  it('creates, reads and saves a mission with optimistic revisions', async () => {
    const created = await settle(api.createMission(exampleMission()));
    expect(created.revision).toBe(1);
    creds.setSession(created.missionId, created.session);
    const got = await settle(api.getMission(created.missionId));
    expect(got.role).toBe('edit');
    expect(got.mission.name).toBe(exampleMission().name);

    const renamed = { ...got.mission, name: 'Renamed' };
    expect(await settle(api.saveMission(created.missionId, renamed, 1))).toBe(2);
    expect((await rejection(api.saveMission(created.missionId, renamed, 1))).status).toBe(409);
    const invalid = { ...renamed, pilot: undefined } as never;
    expect((await rejection(api.saveMission(created.missionId, invalid, 2))).issues.length).toBeGreaterThan(0);

    creds.setSession(created.missionId, null);
    expect((await rejection(api.getMission(created.missionId))).status).toBe(401);
    expect((await rejection(api.getMission('m-unknown'))).status).toBe(404);
  });

  it('shares a mission by PIN, pauses after wrong PINs and honours revocation', async () => {
    const created = await settle(api.createMission(exampleMission()));
    expect((await rejection(api.listLinks(created.missionId, 'not-the-owner'))).status).toBe(403);
    const link = await settle(api.createLink(created.missionId, { role: 'view', pin: '123456', label: 'Crew' }, created.ownerKey));
    const token = (link as unknown as { token: string }).token;
    expect((await settle(api.linkInfo(token))).role).toBe('view');

    const wrong = await rejection(api.openLink(token, '000000'));
    expect(wrong.status).toBe(401);
    expect((wrong.body as { triesLeft: number }).triesLeft).toBe(4);
    const opened = await settle(api.openLink(token, '123456'));
    expect(opened.missionId).toBe(created.missionId);

    for (let i = 0; i < 4; i++) await rejection(api.openLink(token, '999999'));
    const paused = await rejection(api.openLink(token, '999999'));
    expect(paused.status).toBe(429);
    expect(paused.retryAfterS).toBe(900);
    expect((await rejection(api.openLink(token, '123456'))).status).toBe(429);

    const links = await settle(api.listLinks(created.missionId, created.ownerKey));
    expect(links[0].pausedUntil).toBeDefined();
    await settle(api.revokeLink(created.missionId, links[0].linkId, created.ownerKey));
    expect((await rejection(api.linkInfo(token))).status).toBe(410);
    expect((await rejection(api.linkInfo('demo-expired'))).message).toContain('expired');
    expect((await rejection(api.revokeLink(created.missionId, 'l-none', created.ownerKey))).status).toBe(404);
    expect((await rejection(api.createLink(created.missionId, { role: 'view', pin: '123456', expiresAt: '2000-01-01T00:00:00Z' }, created.ownerKey))).status).toBe(400);
  });

  it('runs a coverage job part by part and serves the finished grids', async () => {
    const mission = exampleMission();
    const route = await settle(new Promise((resolve, reject) => api.calcRoute(null, mission).subscribe({ next: resolve, error: reject })));
    expect(route).toHaveProperty('legs');

    const started = await settle(api.startCoverage(null, mission));
    expect(started.state).toBe('running');
    await MockApiHarness.wait(5_000);
    const done: Job = await settle(api.getJob(null, started.jobId!));
    expect(done.state).toBe('done');
    const coverageParts = (done.parts ?? []).filter((p) => p.link);
    expect(coverageParts.map((p) => p.link).sort()).toEqual(['control', 'video']);
    const grid = await settle(api.getCoverage(null, started.jobId!, 'video'));
    expect(grid.cols).toBeGreaterThan(0);

    const cancelled = await settle(api.startCoverage(null, { ...mission, evaluate: 'control' }));
    await settle(api.cancelJob(null, cancelled.jobId!));
    expect((await settle(api.getJob(null, cancelled.jobId!))).state).toBe('cancelled');
    expect((await rejection(api.getJob(null, 'job-none'))).status).toBe(404);
    expect((await rejection(api.getCoverage(null, 'job-none', 'video'))).status).toBe(404);
  });

  it('answers profiles and weather, and refuses a malformed point', async () => {
    const profile = await settle(api.elevationProfile({ lat: 48.54, lon: 35.2 }, { lat: 48.56, lon: 35.23 }));
    expect(profile).toBeTruthy();
    expect(await settle(api.weatherPoint(48.5, 35.2))).toBeTruthy();
    expect(await settle(api.weatherHorizon())).toBeTruthy();
    expect((await settle(api.mapConfig())).zoom).toBe(12);
    expect((await rejection(api.elevationProfile({ lat: Number.NaN, lon: 1 }, { lat: 1, lon: 1 }))).status).toBe(400);
  });

  it('keeps library versions and history for the admin only', async () => {
    const { version: _v, verified: _ok, ...seeded } = LIBRARY.batteries[1];
    const battery = { ...seeded, id: 'test-pack', name: 'Test pack' };
    expect((await rejection(api.addLibraryEntry('batteries', battery))).status).toBe(401);
    creds.setAdminToken('admin-token');
    const added = (await settle(api.addLibraryEntry('batteries', battery))) as { version: number; verified: boolean };
    expect([added.version, added.verified]).toEqual([1, false]);
    expect((await rejection(api.addLibraryEntry('batteries', battery))).status).toBe(409);
    expect((await rejection(api.addLibraryEntry('batteries', { ...battery, id: 'x', cellsSeries: 'four' }))).status).toBe(422);

    const saved = (await settle(api.saveLibraryEntry('batteries', 'test-pack', { ...battery, version: 1, notes: 'checked' }))) as { version: number };
    expect(saved.version).toBe(2);
    expect((await rejection(api.saveLibraryEntry('batteries', 'test-pack', { ...battery, version: 1 }))).status).toBe(409);
    expect((await rejection(api.saveLibraryEntry('batteries', 'test-pack', { ...battery }))).message).toContain('since version ?;');
    expect((await rejection(api.saveLibraryEntry('batteries', 'test-pack', { ...battery, version: { n: 1 } }))).message).toContain('since version {"n":1}; now at version 2');
    expect((await rejection(api.saveLibraryEntry('batteries', 'nope', { ...battery, id: 'nope', version: 1 }))).status).toBe(404);
    expect(await settle(api.libraryHistory('batteries', 'test-pack'))).toHaveLength(1);
    expect((await settle(api.libraryItem('batteries', 'test-pack')))['version']).toBe(2);
    expect((await rejection(api.libraryItem('batteries', 'nope'))).status).toBe(404);
  });

  it('manages modules, keys and cache limits for the admin', async () => {
    creds.setAdminToken('admin-token');
    const off = await settle(api.setModule('fpv.relay', { enabled: false }));
    expect(off.filter((m) => m.id === 'fpv.relay').every((m) => m.state === 'disabled')).toBe(true);
    expect((await rejection(api.setModule('nope', { enabled: true }))).status).toBe(422);
    expect((await rejection(api.removeModule('rf-core'))).message).toContain('Built-in');
    expect((await rejection(api.removeModule('fpv.core'))).message).toContain('Required by');
    await settle(api.removeModule('fpv.example'));
    expect((await rejection(api.removeModule('fpv.example'))).status).toBe(422);
    expect(await settle(api.moduleNotices())).toEqual([]);

    const notZip = await settle(api.uploadModule(new Blob(['nope']), false));
    expect(notZip.ok).toBe(false);
    const zip = await settle(api.uploadModule(new Blob([new Uint8Array([0x50, 0x4b, 3, 4])]), false));
    expect(zip.ok).toBe(true);

    const keys = await settle(api.addKey('rfmod-ed25519 0123456789abcdef', 'Lab'));
    expect(keys.some((k) => k.id === '01234567')).toBe(true);
    expect((await rejection(api.addKey('ssh-rsa AAAA'))).status).toBe(422);
    await settle(api.deleteKey('01234567'));
    expect((await rejection(api.deleteKey('01234567'))).status).toBe(409);
    expect((await settle(api.adminKeys())).length).toBeGreaterThan(0);

    const tiers = await settle(api.adminCache());
    const tableId = tiers.find((t) => t.id === 'results-table')!.id;
    const updated = await settle(api.putAdminCache([{ id: tableId, limitRows: 10 }]));
    expect(updated.find((t) => t.id === tableId)?.limitRows).toBe(10);
    expect((await rejection(api.putAdminCache([{ id: tiers[0].id === tableId ? tiers[1].id : tiers[0].id, limitRows: 1 }]))).status).toBe(400);
    expect((await rejection(api.putAdminCache([{ id: tableId, limitBytes: -1 }]))).status).toBe(400);
    expect((await rejection(api.putAdminCache([{ id: 'nope' as never }]))).status).toBe(400);
    await settle(api.clearCache());
  });
});
