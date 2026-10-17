import { describe, expect, it } from 'vitest';
import { RfFormat, distanceM, groundToAir, polylineLengthM } from '@rfmap/ui-kit';
import { A_TO_B_M, PILOT, TARGET, exampleMission, fixAltitude, routeResult } from './reference-case';

/** The dev mock must reproduce the reference case exactly (leg 3 WP2 → B, Dnipro). */
describe('mock reference case', () => {
  const mission = exampleMission();
  const base = routeResult(mission, 'normal');
  const leg3 = base.legs?.[2];

  it('has the reference geometry', () => {
    // client-side spherical distance is within 0.1 % of the server's ellipsoidal 5280.37 m
    expect(Math.abs(distanceM(PILOT, TARGET) - A_TO_B_M) / A_TO_B_M).toBeLessThan(1e-3);
    expect(RfFormat.format(A_TO_B_M, 'km3')).toBe('5.280 km');
    const route = polylineLengthM([mission.pilot, ...mission.waypoints]);
    expect(RfFormat.format(route, 'km1')).toBe('10.8 km');
  });

  it('video 915 MHz on leg 3 is blocked by the ridge', () => {
    const v = leg3?.worst?.video;
    expect(v?.reachable).toBe(false);
    const fspl = v?.terms?.find((t) => t.id === 'fspl')?.valueDb ?? Number.NaN;
    expect(RfFormat.format(fspl, 'db')).toBe('106.13 dB');
    expect(RfFormat.format(v?.diffraction?.lossDb ?? Number.NaN, 'db')).toBe('29.61 dB');
    expect(RfFormat.format(v?.receivedDbm ?? Number.NaN, 'dbm')).toBe('−98.74 dBm');
    expect(RfFormat.format(v?.requiredDbm ?? Number.NaN, 'dbm')).toBe('−95.00 dBm');
    expect(RfFormat.format(v?.marginDb ?? Number.NaN, 'db+')).toBe('−3.74 dB');
    expect(RfFormat.format(v?.diffraction?.firstFresnelRadiusM ?? Number.NaN, 'm2')).toBe('20.79 m');
    expect(RfFormat.format(v?.diffraction?.nu ?? Number.NaN, 'n2')).toBe('6.80');
  });

  it('control 433 MHz on leg 3 still works', () => {
    const c = leg3?.worst?.control;
    expect(c?.reachable).toBe(true);
    expect(RfFormat.format(c?.diffraction?.lossDb ?? Number.NaN, 'db')).toBe('26.36 dB');
    expect(RfFormat.format(c?.marginDb ?? Number.NaN, 'db+')).toBe('+6.01 dB');
    expect(RfFormat.format(c?.diffraction?.firstFresnelRadiusM ?? Number.NaN, 'm2')).toBe('30.22 m');
  });

  it('the altitude fix gives +25.32 dB video margin', () => {
    const fix = fixAltitude(mission);
    expect(fix.title).toContain('345 m');
    const fixed = routeResult({ ...mission, waypoints: mission.waypoints.map((w, i) => (i === 2 ? { ...w, aglM: 345 } : w)) }, 'normal');
    const v = fixed.legs?.[2]?.worst?.video;
    expect(RfFormat.format(v?.diffraction?.lossDb ?? Number.NaN, 'db')).toBe('0.55 dB');
    expect(RfFormat.format(v?.marginDb ?? Number.NaN, 'db+')).toBe('+25.32 dB');
  });

  it('battery: needs 16.1 km, has 10.2 km, turn back at 6.0 km', () => {
    const b = base.battery;
    expect(RfFormat.format(b?.rangeM ?? Number.NaN, 'km1')).toBe('10.2 km');
    expect(RfFormat.format(b?.neededM ?? Number.NaN, 'km1')).toBe('16.1 km');
    expect(RfFormat.format(b?.turnBack?.distanceAlongRouteM ?? Number.NaN, 'km1')).toBe('6.0 km');
    expect(RfFormat.format(b?.turnBack?.returnM ?? Number.NaN, 'km1')).toBe('4.2 km');
  });

  it('uses the server shapes: 0-based legs, return leg last, reasons with link', () => {
    expect(base.legs?.map((l) => l.index)).toEqual([0, 1, 2, 3]);
    const ret = base.legs?.[3];
    expect([ret?.from, ret?.to]).toEqual(['B', 'A']);
    expect(ret?.worst?.video?.sample?.alongLegM).toBe(0);
    expect(leg3?.reasons?.[0]?.link).toBe('video');
    expect(leg3?.reasons?.[0]?.fix?.id).toBe('leg3-345m-agl');
    // module reasons are route-wide; modules[id] is the module data itself
    expect(base.reasons?.[0]?.module).toBe('fpv.battery');
    expect(base.modules?.['fpv.battery']).not.toHaveProperty('reasons');
    expect(base.forecastTier).toBe('hourly');
    expect(routeResult({ ...mission, modules: {} }, 'normal').forecastTier).toBe('off');
    expect(routeResult({ ...mission, returnToPilot: false }, 'normal').legs?.length).toBe(3);
  });

  it('orders the video profile from the drone and the chart view flips it back', () => {
    const v = leg3!.worst!.video!;
    expect(v.ends?.transmitter).toBe('air');
    expect(v.ends?.ground?.aglM).toBe(2);
    expect(v.ends?.air?.aglM).toBe(120);
    const first = v.profile!.points[0];
    expect(Math.abs(first.lat! - TARGET.lat)).toBeLessThan(1e-6);
    const view = groundToAir(v);
    expect(Math.abs(view.profile!.points[0].lat! - PILOT.lat)).toBeLessThan(1e-6);
    // the ridge is 2.6 km from the pilot whichever way the profile runs
    expect(view.diffraction!.dominant!.distanceM).toBeCloseTo(v.diffraction!.d2M!, 6);
    expect(view.diffraction!.d1M! + view.diffraction!.d2M!).toBeCloseTo(A_TO_B_M, 6);
    expect(leg3?.worst?.control?.ends?.transmitter).toBe('ground');
  });

  it('missing elevation: the leg lists positions with spans and the area bbox', () => {
    const r = routeResult(mission, 'missing-elevation');
    const l = r.legs![2];
    expect(l.status).toBe('missing-data');
    expect(l.missing!.length).toBeGreaterThan(0);
    expect(l.missing!.length).toBeLessThanOrEqual(20);
    for (const p of l.missing!) expect(p.missing![0].fromM!).toBeLessThan(p.missing![0].toM!);
    const [w, s0, e, n] = l.missingData!.bbox!;
    expect(w).toBeLessThan(e);
    expect(s0).toBeLessThan(n);
    expect(l.samples!.missingData).toBeGreaterThan(0);
    expect(l.samples!.evaluated! + l.samples!.missingData!).toBe(l.samples!.total);
  });
});
