import { TestBed } from '@angular/core/testing';
import { Observable, Subject } from 'rxjs';
import { CoverageGrid, CreateMissionResponse, Job, RouteResult } from '../models/api.models';
import { LinkKind, Mission } from '../models/mission.models';
import { ApiError, RfmapApiClient } from './api-client';
import { CredentialStore } from './credential-store';
import { MissionStore, coverageLinkOfPart, partLink } from './mission-store';

function mission(over: Partial<Mission> = {}): Mission {
  return {
    schema: 'mission/v1',
    name: 'Test',
    time: { mode: 'now' },
    pilot: { lat: 48.5, lon: 35.2, aglM: 1.5 },
    waypoints: [{ lat: 48.51, lon: 35.21, aglM: 80 }],
    links: {},
    evaluate: 'both',
    ...over,
  };
}

/** The API calls MissionStore makes, answered from fields a test sets; every call is recorded. */
class FakeApi {
  readonly calls: string[] = [];
  created: CreateMissionResponse = { missionId: 'm1', ownerKey: 'owner-1', session: 'session-1' };
  saveResult: Promise<number> = Promise.resolve(8);
  coverageStart: () => Promise<Job> = () => Promise.resolve({ jobId: 'j1', state: 'running', parts: [] });
  jobs: Job[] = [];
  route = new Subject<RouteResult>();
  coverageFails = new Set<LinkKind>();
  gridsReady: Promise<void> = Promise.resolve();

  createMission(m: Mission): Promise<CreateMissionResponse> {
    this.calls.push(`create ${m.name}`);
    return Promise.resolve(this.created);
  }
  getMission(id: string) {
    this.calls.push(`get ${id}`);
    return Promise.resolve({ mission: mission({ name: 'Loaded' }), role: 'view' as const, revision: 4 });
  }
  saveMission(id: string, _m: Mission, revision: number): Promise<number> {
    this.calls.push(`save ${id}@${revision}`);
    return this.saveResult;
  }
  calcRoute(id: string | null): Observable<RouteResult> {
    this.calls.push(`route ${id}`);
    return this.route;
  }
  startCoverage(id: string | null): Promise<Job> {
    this.calls.push(`coverage ${id}`);
    return this.coverageStart();
  }
  getJob(_id: string | null, jobId: string): Promise<Job> {
    this.calls.push(`job ${jobId}`);
    const next = this.jobs.shift();
    return next ? Promise.resolve(next) : Promise.reject(new ApiError(503, 'job service gone'));
  }
  cancelJob(_id: string | null, jobId: string): Promise<void> {
    this.calls.push(`cancel ${jobId}`);
    return Promise.reject(new ApiError(404, 'already finished'));
  }
  async getCoverage(_id: string | null, jobId: string, link: LinkKind): Promise<CoverageGrid> {
    this.calls.push(`grid ${jobId} ${link}`);
    await this.gridsReady;
    if (this.coverageFails.has(link)) throw new ApiError(500, 'no grid');
    return { link, cols: 2, rows: 2 };
  }
}

/** Lets pending promise callbacks and zero-delay timers run. */
async function settle(rounds = 10): Promise<void> {
  for (let i = 0; i < rounds; i++) await new Promise((r) => setTimeout(r, 0));
}

describe('coverage part links', () => {
  it('prefer the part field and fall back to the display name', () => {
    expect(partLink({ link: 'video', name: 'Coverage grid, control link' })).toBe('video');
    expect(partLink({ name: 'Coverage grid, control link' })).toBe('control');
    expect(coverageLinkOfPart('Coverage grid, VIDEO link')).toBe('video');
    expect(coverageLinkOfPart('Coverage grid')).toBeNull();
    expect(coverageLinkOfPart('Route, control link')).toBeNull();
    expect(coverageLinkOfPart(undefined)).toBeNull();
  });
});

describe('MissionStore', () => {
  let api: FakeApi;
  let creds: CredentialStore;
  let store: MissionStore;

  beforeEach(() => {
    api = new FakeApi();
    TestBed.configureTestingModule({ providers: [{ provide: RfmapApiClient, useValue: api }] });
    creds = TestBed.inject(CredentialStore);
    store = TestBed.inject(MissionStore);
    store.pollIntervalMs = 0;
  });

  describe('editing', () => {
    it('marks a new mission dirty and keeps edits off the original object', () => {
      const m = mission();
      store.newMission(m);
      store.setPilot({ lat: 48.6, lon: 35.3 });
      store.addWaypoint({ lat: 48.7, lon: 35.4, aglM: 50 });
      store.movePoint(0, { lat: 48.52, lon: 35.22 });
      store.setEvaluate('video');
      const now = store.mission()!;
      expect(now.pilot).toEqual({ lat: 48.6, lon: 35.3, aglM: 1.5 });
      expect(now.waypoints.map((w) => w.lat)).toEqual([48.52, 48.7]);
      expect(store.evaluated()).toEqual(['video']);
      expect(store.dirty()).toBe(true);
      expect(m.pilot.lat).toBe(48.5);
    });

    it('moves the pilot for index -1 and never removes the last waypoint', () => {
      store.newMission(mission());
      store.movePoint(-1, { lat: 1, lon: 2 });
      expect(store.mission()!.pilot.lat).toBe(1);
      store.removeWaypoint(0);
      expect(store.mission()!.waypoints.length).toBe(1);
      store.addWaypoint({ lat: 3, lon: 4, aglM: 10 });
      store.removeWaypoint(0);
      expect(store.mission()!.waypoints).toEqual([{ lat: 3, lon: 4, aglM: 10 }]);
    });

    it('ignores edits without a mission or for a view-only link', async () => {
      store.setPilot({ lat: 1, lon: 1 });
      expect(store.mission()).toBeNull();
      await store.load('m9');
      expect(api.calls).toEqual(['get m9']);
      expect(store.canEdit()).toBe(false);
      expect(store.revision()).toBe(4);
      store.setPilot({ lat: 1, lon: 1 });
      expect(store.mission()!.pilot.lat).toBe(48.5);
      expect(store.dirty()).toBe(false);
    });

    it('drops coverage when the pilot moves and keeps it when a waypoint moves', () => {
      store.loadFromServer('m1', { mission: mission(), role: 'edit', revision: 2, lastResult: { legs: [] } });
      store.coverage.set({ control: { link: 'control', cols: 2, rows: 2 } });
      store.movePoint(0, { lat: 48.52, lon: 35.22 });
      expect(store.coverage().control).toBeDefined();
      expect(store.pilotStale()).toBe(false);
      store.setPilotAgl(3);
      expect(store.mission()!.pilot.aglM).toBe(3);
      expect(store.coverage()).toEqual({});
      expect(store.pilotStale()).toBe(true);
      expect(store.resultStale()).toBe(true);
    });

    it('marks the pilot stale when only coverage was there', () => {
      store.newMission(mission());
      store.coverage.set({ control: { link: 'control', cols: 2, rows: 2 } });
      store.setPilot({ lat: 48.6, lon: 35.3 });
      expect(store.coverage()).toEqual({});
      expect(store.pilotStale()).toBe(true);
    });

    it('undoes the last move with its result and coverage, without a recalculation', () => {
      const grid = { link: 'control' as const, cols: 2, rows: 2 };
      store.loadFromServer('m1', { mission: mission(), role: 'edit', revision: 2, lastResult: { legs: [] } });
      store.coverage.set({ control: grid });
      expect(store.canUndoMove()).toBe(false);
      store.setPilot({ lat: 48.6, lon: 35.3 });
      expect(store.canUndoMove()).toBe(true);
      expect(store.dirty()).toBe(true);
      store.undoMove();
      expect(store.mission()!.pilot).toEqual({ lat: 48.5, lon: 35.2, aglM: 1.5 });
      expect(store.coverage()).toEqual({ control: grid });
      expect(store.result()).toEqual({ legs: [] });
      expect([store.dirty(), store.resultStale(), store.pilotStale(), store.canUndoMove()]).toEqual([false, false, false, false]);
      expect(api.calls).toEqual([]);
    });

    it('forgets the undo after any other edit', () => {
      store.newMission(mission());
      store.movePoint(0, { lat: 1, lon: 2 });
      store.setEvaluate('video');
      expect(store.canUndoMove()).toBe(false);
      store.undoMove();
      expect(store.mission()!.waypoints[0].lat).toBe(1);
    });

    it('does not move points while a calculation runs', async () => {
      store.newMission(mission());
      void store.calculate();
      await settle();
      expect(store.busy()).toBe(true);
      store.movePoint(-1, { lat: 1, lon: 2 });
      store.setPilotAgl(9);
      expect(store.mission()!.pilot).toEqual({ lat: 48.5, lon: 35.2, aglM: 1.5 });
      expect(store.canUndoMove()).toBe(false);
    });

    it('does not move the pilot while a fix is previewed', () => {
      store.newMission(mission());
      store.previewFix({ id: 'f', patch: { name: 'Fixed' } });
      store.movePoint(-1, { lat: 1, lon: 2 });
      store.setPilotAgl(9);
      expect(store.mission()!.pilot).toEqual({ lat: 48.5, lon: 35.2, aglM: 1.5 });
    });

    it('undo drops a fix preview built on the moved mission', () => {
      store.newMission(mission());
      store.setPilot({ lat: 48.6, lon: 35.3 });
      store.previewFix({ id: 'f', patch: { name: 'Fixed' } });
      store.undoMove();
      expect(store.preview()).toBeNull();
      expect(store.mission()!.pilot.lat).toBe(48.5);
    });

    it('does not let a grid that arrives after a pilot move bring the old coverage back', async () => {
      let release!: () => void;
      api.gridsReady = new Promise((r) => (release = r));
      api.coverageStart = () => Promise.resolve({ jobId: 'j1', state: 'done', parts: [{ link: 'control', state: 'done' }] });
      store.newMission(mission());
      await store.calculate();
      api.route.next({ legs: [] });
      await settle();
      expect(store.busy()).toBe(false);
      store.setPilot({ lat: 48.6, lon: 35.3 });
      release();
      await settle();
      expect(store.coverage()).toEqual({});
      expect(store.pilotStale()).toBe(true);
    });

    it('marks an existing result stale on the first edit', () => {
      store.loadFromServer('m1', { mission: mission(), role: 'edit', revision: 2, lastResult: { legs: [] } });
      expect(store.calcState()).toBe('done');
      store.setEvaluate('control');
      expect(store.resultStale()).toBe(true);
    });

    it('clears back to the empty state', () => {
      store.loadFromServer('m1', { mission: mission(), role: 'view', revision: 2 });
      store.clear();
      expect([store.mission(), store.missionId(), store.revision(), store.role()]).toEqual([null, null, 0, 'edit']);
    });
  });

  describe('fixes', () => {
    it('shows the patched mission only while previewing', () => {
      store.newMission(mission());
      store.previewFix({ id: 'no-patch' });
      expect(store.preview()).toBeNull();
      store.previewFix({ id: 'raise', patch: { waypoints: [{ lat: 48.51, lon: 35.21, aglM: 120 }] } });
      expect(store.shownMission()!.waypoints[0].aglM).toBe(120);
      expect(store.mission()!.waypoints[0].aglM).toBe(80);
      store.cancelPreview();
      expect(store.shownMission()!.waypoints[0].aglM).toBe(80);
    });

    it('applies the preview and recalculates', async () => {
      store.newMission(mission());
      store.previewFix({ id: 'name', patch: { name: 'Fixed' } });
      await store.applyPreview();
      expect(store.mission()!.name).toBe('Fixed');
      expect(store.preview()).toBeNull();
      expect(api.calls).toContain('create Fixed');
    });
  });

  describe('calculation', () => {
    it('creates the mission, keeps its credentials and finishes when route and coverage are done', async () => {
      api.coverageStart = () =>
        Promise.resolve({ jobId: 'j1', state: 'running', parts: [{ name: 'Coverage grid, control link', state: 'done' }] });
      api.jobs = [
        { jobId: 'j1', state: 'running', parts: [{ name: 'Coverage grid, control link', state: 'done' }, { link: 'video', state: 'running' }] },
        { jobId: 'j1', state: 'done', parts: [{ name: 'Coverage grid, control link', state: 'done' }, { link: 'video', state: 'cached' }] },
      ];
      store.newMission(mission());
      const run = store.calculate();
      await run;
      expect(store.calcState()).toBe('running');
      expect(creds.ownerKey('m1')).toBe('owner-1');
      expect(creds.session('m1')).toBe('session-1');
      expect(creds.lastMissionId()).toBe('m1');
      expect(store.revision()).toBe(1);

      api.route.next({ legs: [] });
      await settle();
      expect(store.calcState()).toBe('done');
      expect(store.result()).toEqual({ legs: [] });
      // Each link's grid is fetched once, as soon as its part is done.
      expect(api.calls.filter((c) => c.startsWith('grid'))).toEqual(['grid j1 control', 'grid j1 video']);
      expect(Object.keys(store.coverage()).sort()).toEqual(['control', 'video']);
    });

    it('saves a dirty loaded mission with its revision', async () => {
      store.loadFromServer('m2', { mission: mission(), role: 'edit', revision: 7 });
      store.setEvaluate('control');
      await store.calculate();
      expect(api.calls.slice(0, 2)).toEqual(['save m2@7', 'route m2']);
      expect(store.revision()).toBe(8);
      expect(store.dirty()).toBe(false);
    });

    it('explains a save conflict and stops', async () => {
      api.saveResult = Promise.reject(new ApiError(409, 'conflict'));
      store.loadFromServer('m2', { mission: mission(), role: 'edit', revision: 7 });
      store.setEvaluate('control');
      await store.calculate();
      expect(store.calcState()).toBe('error');
      expect(store.error()).toContain('Someone saved this mission in between');
      expect(api.calls).not.toContain('route m2');
    });

    it('reports a failed route request', async () => {
      store.newMission(mission());
      await store.calculate();
      api.route.error(new ApiError(422, 'missing terrain'));
      await settle();
      expect(store.calcState()).toBe('error');
      expect(store.error()).toBe('missing terrain');
    });

    it('keeps the route verdict when coverage cannot start', async () => {
      api.coverageStart = () => Promise.reject(new ApiError(503, 'coverage busy'));
      store.newMission(mission());
      await store.calculate();
      api.route.next({ legs: [] });
      await settle();
      expect(store.calcState()).toBe('done');
      expect(store.error()).toBe('Coverage: coverage busy');
    });

    it('says when the coverage job failed or polling broke', async () => {
      api.jobs = [{ jobId: 'j1', state: 'failed', parts: [] }];
      store.newMission(mission());
      await store.calculate();
      await settle();
      expect(store.error()).toBe('Coverage job failed; route results are still valid.');

      api.jobs = [];
      await store.calculate();
      await settle();
      expect(store.error()).toBe('Coverage: job service gone');
    });

    it('retries a coverage grid that could not be fetched', async () => {
      api.coverageFails.add('control');
      const part = { link: 'control' as const, state: 'done' as const };
      api.jobs = [
        { jobId: 'j1', state: 'running', parts: [part] },
        { jobId: 'j1', state: 'done', parts: [part] },
      ];
      store.newMission(mission());
      await store.calculate();
      await settle();
      expect(api.calls.filter((c) => c === 'grid j1 control').length).toBeGreaterThan(1);
      expect(store.coverage()).toEqual({});
    });

    it('does nothing without a mission', async () => {
      await store.calculate();
      expect(api.calls).toEqual([]);
      expect(store.calcState()).toBe('idle');
    });

    it('cancels the running job and ignores late answers', async () => {
      api.jobs = [{ jobId: 'j1', state: 'running', parts: [] }];
      store.newMission(mission());
      await store.calculate();
      await store.cancel();
      expect(store.calcState()).toBe('cancelled');
      expect(api.calls).toContain('cancel j1');
      api.route.next({ legs: [] });
      await settle();
      expect(store.result()).toBeNull();
      expect(store.calcState()).toBe('cancelled');
    });
  });
});
