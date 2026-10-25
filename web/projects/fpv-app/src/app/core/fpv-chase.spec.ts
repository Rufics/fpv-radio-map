import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore, distanceM, initialBearingDeg } from '@rfmap/ui-kit';
import { fpvAltitudeMock } from '../mock/fpv-altitude-mock';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { AltitudeView, Corridor, FPV_ALTITUDE } from './fpv-altitude';
import { CHASE_DEFAULT_SPEED_MPS, ChaseTrack, ChaseView } from './fpv-chase';
import { FpvDataService } from './fpv-data.service';

const mission = { ...exampleMission(), modules: { [FPV_ALTITUDE]: {} } };

function track(speed = 10): ChaseTrack {
  const data = fpvAltitudeMock(mission);
  return new ChaseTrack(new Corridor(data.corridor!, data.pilot!), data.waypoints ?? [], speed);
}

describe('ChaseTrack', () => {
  const t = track();
  const c = t.corridor.c;

  it('flies the route at a steady ground speed', () => {
    expect(t.length).toBe(c.s.at(-1));
    expect(t.durationS).toBeCloseTo(t.length / 10, 6);
    expect(t.sAt(60)).toBe(600);
    expect(t.sAt(-5)).toBe(0);
    expect(t.sAt(1e9)).toBe(t.length);
  });

  it('reads the plan at a point: heights, state, leg and the climb it asks for', () => {
    const i = 20;
    const f = t.frame(c.s[i]);
    expect(f.mslM).toBeCloseTo(c.plannedMslM[i], 6);
    expect(f.aglM).toBeCloseTo(c.plannedMslM[i] - c.groundMslM[i], 6);
    expect(f.needAglM).toBeCloseTo(c.floorMslM[i] - c.groundMslM[i], 6);
    expect(f.leg).toBe(c.leg[i]);
    expect(f.state).toBe(t.corridor.state(i));
    expect(f.timeS).toBeCloseTo(c.s[i] / 10, 6);
    // the slope over ±50 m at 10 m/s
    const a = c.s[i] - 50;
    const b = c.s[i] + 50;
    const slope =
      (t.corridor.valueAt('plannedMslM', b) - t.corridor.valueAt('plannedMslM', a)) / 100;
    expect(f.verticalMps).toBeCloseTo(slope * 10, 6);
  });

  it('heads along the leg it is on', () => {
    const wps = fpvAltitudeMock(mission).waypoints!;
    const [w1, w2] = wps;
    const mid = (w1.s + w2.s) / 2;
    const f = t.frame(mid);
    const p1 = {
      lat: c.lat[t.corridor.indexAt(w1.s + 200)],
      lon: c.lon[t.corridor.indexAt(w1.s + 200)],
    };
    const p2 = {
      lat: c.lat[t.corridor.indexAt(w2.s - 200)],
      lon: c.lon[t.corridor.indexAt(w2.s - 200)],
    };
    const leg = initialBearingDeg(p1, p2);
    const diff = Math.abs(((f.headingDeg - leg + 540) % 360) - 180);
    expect(diff).toBeLessThan(5);
  });

  it('puts the camera behind, a little to the left of and above the drone, looking at the ground ahead', () => {
    const s = 5000;
    const f = t.frame(s);
    const cam = t.camera(s, 2);
    // 300 m back and 60 m to the left: 11.3° left of straight behind
    const back = initialBearingDeg(f, cam.from);
    const off = ((back - (f.headingDeg + 180) + 540) % 360) - 180;
    expect(off).toBeCloseTo((Math.atan2(60, 300) * 180) / Math.PI, 0);
    expect(distanceM(f, cam.from)).toBeCloseTo(Math.hypot(300, 60), 0);
    expect(cam.fromAltM).toBeCloseTo(f.mslM * 2 + 150, 6);
    expect(distanceM(f, cam.to)).toBeCloseTo(500, 0);
    expect(cam.toAltM).toBeCloseTo(t.corridor.valueAt('groundMslM', s + 500) * 2, 6);
  });

  it('keeps the camera over the ground behind a climb', () => {
    const data = fpvAltitudeMock(mission);
    const cor = data.corridor!;
    // the ground rises steeply behind the drone, far over the plan
    const steep = {
      ...cor,
      groundMslM: cor.groundMslM.map((g, i) => (cor.s[i] < 4750 ? g + 900 : g)),
    };
    const tt = new ChaseTrack(new Corridor(steep, data.pilot!), data.waypoints ?? [], 10);
    const cam = tt.camera(5000, 1);
    expect(cam.fromAltM).toBeGreaterThanOrEqual(
      tt.corridor.valueAt('groundMslM', 4700) + 25 - 1e-6,
    );
  });

  it('names the next waypoint, then the pilot at the end of a return', () => {
    const wps = fpvAltitudeMock(mission).waypoints!;
    const [first] = t.ahead(0);
    expect(first.kind).toBe('waypoint');
    expect(first.text).toBe(wps[0].label ?? 'WP1');
    expect(first.inM).toBeCloseTo(wps[0].s, 6);
    const last = t.ahead(wps.at(-1)!.s + 10)[0];
    expect(last.text).toBe('Pilot');
    expect(t.ahead(t.length)).toEqual([]);
  });

  it('warns where the plan leaves the corridor ahead', () => {
    const data = fpvAltitudeMock(mission);
    const cor = data.corridor!;
    const i = cor.s.findIndex((s) => s > 3000);
    const low = {
      ...cor,
      plannedMslM: cor.plannedMslM.map((v, k) =>
        k === i ? cor.floorMslM[k] - 20 : Math.max(v, cor.floorMslM[k]),
      ),
    };
    const lowCeil = {
      ...low,
      ceilingMslM: low.ceilingMslM.map((v, k) => Math.max(v, low.plannedMslM[k])),
    };
    const tt = new ChaseTrack(new Corridor(lowCeil, data.pilot!), data.waypoints ?? [], 10);
    const warn = tt.ahead(cor.s[i] - 500).find((a) => a.kind === 'corridor');
    expect(warn?.inM).toBeCloseTo(500, 6);
    expect(warn?.text).toBe(
      `Under the floor: needs ${Math.round(cor.floorMslM[i] - cor.groundMslM[i])} m AGL`,
    );
    // already under it: nothing new to warn about until it gets out
    expect(tt.ahead(cor.s[i]).some((a) => a.kind === 'corridor')).toBe(false);
  });

  it('splits the timeline into the legs, end to end, with the worst state on each', () => {
    const legs = t.legs;
    expect(legs[0].fromS).toBe(0);
    expect(legs.at(-1)!.toS).toBe(t.length);
    for (let k = 1; k < legs.length; k++) expect(legs[k].fromS).toBe(legs[k - 1].toS);
    expect(legs[0].name.startsWith('Pilot → ')).toBe(true);
    expect(legs.at(-1)!.name.endsWith('→ Pilot')).toBe(true);
    for (const l of legs) {
      const states = c.s
        .map((_, i) => i)
        .filter((i) => c.leg[i] === l.leg)
        .map((i) => t.corridor.state(i));
      const want = states.includes('ground')
        ? 'bad'
        : states.some((s) => s === 'low' || s === 'high')
          ? 'warn'
          : 'ok';
      expect(l.tone).toBe(want);
    }
  });
});

describe('ChaseView', () => {
  let chase: ChaseView;
  let view: AltitudeView;

  beforeEach(async () => {
    MockApiHarness.setUp();
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    const store = TestBed.inject(MissionStore);
    store.newMission(mission);
    store.result.set({
      ...routeResult(mission, 'normal'),
      modules: { [FPV_ALTITUDE]: fpvAltitudeMock(mission) },
    });
    chase = TestBed.inject(ChaseView);
    view = TestBed.inject(AltitudeView);
  });

  afterEach(() => MockApiHarness.tearDown());

  it('flies at the climb assistant airspeed, or 15 m/s', () => {
    const climb = view.climb();
    expect(chase.track()!.speedMps).toBe(climb?.airspeedMps || CHASE_DEFAULT_SPEED_MPS);
    expect(chase.limits()).toEqual({
      climbMps: climb!.climbRateMps,
      descentMps: climb!.descentRateMps,
    });
  });

  it('starts in 3D from the profile cursor and moves the cursor with it', () => {
    view.cursorS.set(2000);
    chase.start();
    expect(view.mode()).toBe('3d');
    expect(chase.active()).toBe(true);
    expect(chase.playing()).toBe(true);
    expect(chase.s()).toBe(2000);
    const speed = chase.track()!.speedMps;
    chase.rate.set(4);
    chase.tick(2);
    expect(chase.s()).toBeCloseTo(2000 + 2 * speed * 4, 6);
    expect(view.cursorS()).toBe(chase.s());
    expect(chase.frame()?.s).toBe(chase.s());
    expect(chase.ahead().length).toBeGreaterThan(0);
  });

  it('stops at the end, and plays again from the start', () => {
    chase.start();
    const t = chase.track()!;
    chase.tick(t.durationS);
    expect(chase.s()).toBe(t.length);
    expect(chase.playing()).toBe(false);
    chase.tick(5);
    expect(chase.s()).toBe(t.length);
    chase.toggle();
    expect(chase.s()).toBe(0);
    expect(chase.playing()).toBe(true);
    chase.toggle();
    expect(chase.playing()).toBe(false);
    chase.stop();
    expect(chase.active()).toBe(false);
    expect(chase.frame()).toBeNull();
  });

  it('does not replay a plan from before the mission changed, and ends a running chase', () => {
    const store = TestBed.inject(MissionStore);
    chase.start();
    store.resultStale.set(true);
    expect(chase.track()).toBeNull();
    TestBed.tick();
    expect(chase.active()).toBe(false);
    chase.start();
    expect(chase.active()).toBe(false);
  });

  it('does nothing without a corridor', () => {
    TestBed.inject(MissionStore).result.set(null);
    chase.start();
    expect(chase.track()).toBeNull();
    expect(chase.active()).toBe(false);
    chase.toggle();
    chase.seek(100);
    chase.tick(1);
    expect(chase.s()).toBe(0);
  });
});
