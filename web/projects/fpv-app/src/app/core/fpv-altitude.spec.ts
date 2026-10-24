import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore } from '@rfmap/ui-kit';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { corridorOf } from '../tests/altitude-fixtures';
import {
  AltitudeView,
  Corridor,
  FPV_ALTITUDE,
  fpvAltitudeData,
  fpvAltitudeOptions,
} from './fpv-altitude';
import { FpvDataService } from './fpv-data.service';

describe('Corridor', () => {
  const c = new Corridor(corridorOf(), { lat: 48.5, lon: 35.2 });

  it('says where the plan is against the corridor', () => {
    expect(c.state(0)).toBe('takeoff');
    expect(c.state(1)).toBe('inside');
    expect(c.state(2)).toBe('low');
    expect(c.state(3)).toBe('high');
    const under = new Corridor(corridorOf({ plannedMslM: [501.5, 600, 580, 600] }), {
      lat: 48.5,
      lon: 35.2,
    });
    expect(
      new Corridor(corridorOf({ floorMslM: [500, 550, 620, 480] }), { lat: 48.5, lon: 35.2 }).state(
        0,
      ),
    ).toBe('inside');
    expect(under.state(2)).toBe('ground');
  });

  it('interpolates between samples and finds the sample before a distance', () => {
    expect(c).toHaveLength(3000);
    expect(c.indexAt(-5)).toBe(0);
    expect(c.indexAt(1500)).toBe(1);
    expect(c.indexAt(3000)).toBe(3);
    expect(c.valueAt('groundMslM', 1500)).toBeCloseTo(555, 9);
    expect(c.valueAt('groundMslM', 9000)).toBe(450);
  });
});

/** The altitude view against the dev mock backend, with the app's own store and API client. */
describe('AltitudeView', () => {
  let store: MissionStore;
  let view: AltitudeView;

  beforeEach(async () => {
    MockApiHarness.setUp();
    store = TestBed.inject(MissionStore);
    view = TestBed.inject(AltitudeView);
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    store.newMission(exampleMission());
  });

  afterEach(() => MockApiHarness.tearDown());

  it('reads its options and data from the mission and the result', () => {
    expect(fpvAltitudeOptions(null)).toBeNull();
    expect(fpvAltitudeData(null)).toBeNull();
    expect(fpvAltitudeData({ modules: { [FPV_ALTITUDE]: { kFactor: 1.33 } } })).toEqual({
      kFactor: 1.33,
    });
    expect(view.available()).toBe(true);
    expect(view.enabled()).toBe(false);
    expect(view.corridor()).toBeNull();
  });

  it('opens the profile with the 3D view and starts in 2D at ×2', () => {
    expect(view.mode()).toBe('2d');
    expect(view.exaggeration()).toBe(2);
    expect(view.fresnelShown()).toBe(false);
    view.setMode('3d');
    expect(view.profileOpen()).toBe(true);
  });

  it('opts the mission in and draws the corridor of the result', async () => {
    await MockApiHarness.settle(view.enable());
    await MockApiHarness.wait(6_000);
    expect(store.mission()!.modules?.[FPV_ALTITUDE]).toEqual({});
    expect(view.enabled()).toBe(true);
    const c = view.corridor()!;
    expect(c.c.s.length).toBeGreaterThan(100);
    expect(view.result()?.reference).toBe('agl');
    expect(view.stale()).toBe(false);

    view.setOptions({ linkFloor: false });
    expect(store.mission()!.modules?.[FPV_ALTITUDE]).toEqual({ linkFloor: false });
    expect(view.stale()).toBe(true);
  });
});
