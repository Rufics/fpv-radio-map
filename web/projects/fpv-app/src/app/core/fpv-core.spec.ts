import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore } from '@rfmap/ui-kit';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { CompareService, FPV_CORE, fpvCoreData, fpvCoreOptions } from './fpv-core';
import { FpvDataService } from './fpv-data.service';

const betterVideo = { links: { video: { groundAntenna: 'patch-8dbi-915' } } };

/** Scenario comparison against the dev mock backend, with the app's own store and API client. */
describe('CompareService', () => {
  let store: MissionStore;
  let cmp: CompareService;

  async function calculated(): Promise<void> {
    await MockApiHarness.settle(store.calculate());
    await MockApiHarness.wait(6_000);
    expect(store.calcState()).toBe('done');
  }

  beforeEach(async () => {
    MockApiHarness.setUp();
    store = TestBed.inject(MissionStore);
    cmp = TestBed.inject(CompareService);
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    store.newMission(exampleMission());
  });

  afterEach(() => MockApiHarness.tearDown());

  it('reads its options and data from the mission and the result', () => {
    expect(fpvCoreOptions(null)).toEqual({});
    expect(fpvCoreData(null)).toBeNull();
    expect(fpvCoreData({ modules: { [FPV_CORE]: { export: {} } } })).toEqual({ export: {} });
    expect(cmp.available()).toBe(true);
    expect(cmp.options()).toBeNull();
    expect(cmp.missionB()).toBeNull();
  });

  it('runs scenario B through fpv.core and drops it again', async () => {
    await MockApiHarness.settle(cmp.compare('', betterVideo));
    expect(cmp.options()?.name).toBe('B');
    const b = cmp.missionB()!;
    expect(b.links.video?.groundAntenna).toBe('patch-8dbi-915');
    expect(b.modules?.[FPV_CORE]).toEqual({});
    await MockApiHarness.wait(2_000);
    expect(cmp.result()?.links?.video?.deltaWorstDb).toBeCloseTo(3, 6);

    cmp.remove();
    expect(store.mission()!.modules?.[FPV_CORE]).toBeUndefined();
    expect(cmp.missionB()).toBeNull();
  });

  it('keeps other fpv.core options when B is removed', async () => {
    store.update((m) => ({ ...m, modules: { ...m.modules, [FPV_CORE]: { export: ['kml'] } } }));
    await MockApiHarness.settle(cmp.compare('Patch', betterVideo));
    cmp.remove();
    expect(store.mission()!.modules?.[FPV_CORE]).toEqual({ export: ['kml'] });
  });

  it('needs coverage of A before it can show a difference map', async () => {
    await MockApiHarness.settle(cmp.compare('Patch', betterVideo));
    store.coverage.set({});
    await MockApiHarness.settle(cmp.coverageDiff('video'));
    expect(cmp.diffState()).toBe('failed');
    expect(cmp.diffError()).toContain('coverage of this mission first');
  });

  it('subtracts the coverage grids of B and A cell by cell', async () => {
    await MockApiHarness.settle(cmp.compare('Patch', betterVideo));
    await calculated();
    expect(store.coverage().video).toBeDefined();
    await MockApiHarness.settle(cmp.coverageDiff('video'));
    expect(cmp.diffError()).toBeNull();
    expect(cmp.diffState()).toBe('done');
    expect(cmp.delta()?.alignment).not.toBe('none');
    expect(cmp.link()).toBe('video');
  });

  it('stops a difference run that was cleared meanwhile', async () => {
    await MockApiHarness.settle(cmp.compare('Patch', betterVideo));
    await calculated();
    const run = cmp.coverageDiff('control');
    await MockApiHarness.wait(500);
    expect(cmp.diffState()).toBe('running');
    cmp.clearDiff();
    await MockApiHarness.settle(run);
    expect(cmp.diffState()).toBe('idle');
    expect(cmp.delta()).toBeNull();
  });
});
