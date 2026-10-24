import { describe, expect, it } from 'vitest';
import { MissionAltitude } from '@rfmap/ui-kit';
import { fpvAltitudeMock, fpvAltitudePrepareMock } from './fpv-altitude-mock';
import { elevation } from './mock-terrain';
import { exampleMission } from './reference-case';

describe('fpvAltitudeMock', () => {
  const m = { ...exampleMission(), modules: { 'fpv.altitude': {} } };
  const d = fpvAltitudeMock(m);
  const c = d.corridor!;

  it('samples every step and every point, in parallel arrays', () => {
    const n = c.s.length;
    for (const key of [
      'lat',
      'lon',
      'leg',
      'groundMslM',
      'plannedMslM',
      'floorMslM',
      'ceilingMslM',
      'fresnelRadiusM',
    ] as const)
      expect(c[key]).toHaveLength(n);
    expect(c.s[0]).toBe(0);
    expect(c.s.every((s, i) => i === 0 || s > c.s[i - 1])).toBe(true);
    expect(d.waypoints!.map((w) => w.label ?? '')).toEqual(m.waypoints.map((w) => w.label ?? ''));
  });

  it('gives every waypoint in both references', () => {
    const launch = elevation(m.pilot.lat, m.pilot.lon);
    const target = d.waypoints!.at(-1)!;
    expect(target.aglM).toBeCloseTo(MissionAltitude.height(m, m.waypoints.at(-1)!), 6);
    expect(target.mslM).toBeCloseTo(launch + target.aboveLaunchM, 6);
    expect(d.reference).toBe('agl');
    expect(c.floorMslM.every((f, i) => f >= c.terrainFloorMslM[i])).toBe(true);
    expect(c.ceilingMslM[5] - c.groundMslM[5]).toBe(120);
    expect(d.fresnel?.link).toBe('control');
  });

  it('flies a launch-referenced plan level between equal waypoints', () => {
    const launch = elevation(m.pilot.lat, m.pilot.lon);
    const level = {
      ...m,
      altitudeRef: 'launch' as const,
      waypoints: m.waypoints.map(({ aglM: _agl, ...w }) => ({ ...w, altM: 120 })),
    };
    const lc = fpvAltitudeMock(level).corridor!;
    const onLeg2 = lc.s.map((_, i) => i).filter((i) => lc.leg[i] === 1);
    expect(onLeg2.every((i) => Math.abs(lc.plannedMslM[i] - (launch + 120)) < 1e-6)).toBe(true);
  });

  it('leaves the link floor out when asked', () => {
    const off = fpvAltitudeMock({
      ...m,
      modules: { 'fpv.altitude': { linkFloor: false } },
    }).corridor!;
    expect(off.linkFloorMslM.every((v) => v === null)).toBe(true);
  });

  it('keeps the step inside the module bounds', () => {
    const zero = fpvAltitudeMock({ ...m, modules: { 'fpv.altitude': { stepM: 0 } } });
    expect(zero.corridor!.stepM).toBe(10);
    const nan = fpvAltitudeMock({ ...m, modules: { 'fpv.altitude': { stepM: Number.NaN } } });
    expect(nan.corridor!.stepM).toBe(100);
  });
});

describe('fpvAltitudePrepareMock', () => {
  const m = { ...exampleMission(), modules: { 'fpv.altitude': {} } };

  it('sets only the heights left to the server, at least the clearance above the ground', () => {
    const waypoints = m.waypoints.map((w, i) =>
      i === 0 ? MissionAltitude.withAutoHeight(w) : { ...w, heightSource: 'manual' as const },
    );
    const { mission, autoHeights } = fpvAltitudePrepareMock({ ...m, waypoints });
    expect(autoHeights.map((h) => h.index)).toEqual([0]);
    const h = autoHeights[0];
    expect(h.field).toBe('aglM');
    expect(h.heightM).toBeGreaterThanOrEqual(30);
    expect(h.heightM).toBeLessThanOrEqual(120);
    expect(mission.waypoints[0].aglM).toBe(h.heightM);
    expect(mission.waypoints.slice(1)).toEqual(waypoints.slice(1));
  });

  it('does nothing with every height manual or autoHeight off', () => {
    expect(fpvAltitudePrepareMock(m).autoHeights).toEqual([]);
    const auto = { ...m, waypoints: m.waypoints.map((w) => MissionAltitude.withAutoHeight(w)) };
    expect(
      fpvAltitudePrepareMock({ ...auto, modules: { 'fpv.altitude': { autoHeight: false } } })
        .autoHeights,
    ).toEqual([]);
    expect(fpvAltitudePrepareMock(auto).autoHeights).toHaveLength(m.waypoints.length);
  });
});
