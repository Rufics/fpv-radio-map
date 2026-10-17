import { describe, expect, it } from 'vitest';
import { RfFormat, distanceM, polylineLengthM } from '@rfmap/ui-kit';
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
    const fspl = v?.terms?.find((t) => t.id === 'fspl')?.valueDb ?? NaN;
    expect(RfFormat.format(fspl, 'db')).toBe('106.13 dB');
    expect(RfFormat.format(v?.diffraction?.lossDb ?? NaN, 'db')).toBe('29.61 dB');
    expect(RfFormat.format(v?.receivedDbm ?? NaN, 'dbm')).toBe('−98.74 dBm');
    expect(RfFormat.format(v?.requiredDbm ?? NaN, 'dbm')).toBe('−95.00 dBm');
    expect(RfFormat.format(v?.marginDb ?? NaN, 'db+')).toBe('−3.74 dB');
    expect(RfFormat.format(v?.diffraction?.firstFresnelRadiusM ?? NaN, 'm2')).toBe('20.79 m');
    expect(RfFormat.format(v?.diffraction?.nu ?? NaN, 'n2')).toBe('6.80');
  });

  it('control 433 MHz on leg 3 still works', () => {
    const c = leg3?.worst?.control;
    expect(c?.reachable).toBe(true);
    expect(RfFormat.format(c?.diffraction?.lossDb ?? NaN, 'db')).toBe('26.36 dB');
    expect(RfFormat.format(c?.marginDb ?? NaN, 'db+')).toBe('+6.01 dB');
    expect(RfFormat.format(c?.diffraction?.firstFresnelRadiusM ?? NaN, 'm2')).toBe('30.22 m');
  });

  it('the altitude fix gives +25.32 dB video margin', () => {
    const fix = fixAltitude(mission);
    expect(fix.title).toContain('345 m');
    const fixed = routeResult({ ...mission, waypoints: mission.waypoints.map((w, i) => (i === 2 ? { ...w, aglM: 345 } : w)) }, 'normal');
    const v = fixed.legs?.[2]?.worst?.video;
    expect(RfFormat.format(v?.diffraction?.lossDb ?? NaN, 'db')).toBe('0.55 dB');
    expect(RfFormat.format(v?.marginDb ?? NaN, 'db+')).toBe('+25.32 dB');
  });

  it('battery: needs 16.1 km, has 10.2 km, turn back at 6.0 km', () => {
    const b = base.battery;
    expect(RfFormat.format(b?.rangeM ?? NaN, 'km1')).toBe('10.2 km');
    expect(RfFormat.format(b?.neededM ?? NaN, 'km1')).toBe('16.1 km');
    expect(RfFormat.format(b?.turnBack?.distanceAlongRouteM ?? NaN, 'km1')).toBe('6.0 km');
    expect(RfFormat.format(b?.turnBack?.returnM ?? NaN, 'km1')).toBe('4.2 km');
  });
});
