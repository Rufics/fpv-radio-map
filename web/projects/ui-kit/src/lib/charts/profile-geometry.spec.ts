import { describe, expect, it } from 'vitest';
import { computeProfileGeometry, fresnelRadius, niceStep } from './profile-geometry';

describe('niceStep', () => {
  it('rounds the raw step to 1, 2, 5 or 10 times a power of ten', () => {
    expect(niceStep(400)).toBe(100); // raw 100 → mantissa 1
    expect(niceStep(1000)).toBe(200); // raw 250 → mantissa 2.5 → 2
    expect(niceStep(2000)).toBe(500); // raw 500 → mantissa 5
    expect(niceStep(3200)).toBe(1000); // raw 800 → mantissa 8 → 10
  });

  it('honours the requested tick count and never divides by less than one', () => {
    expect(niceStep(300, 3)).toBe(100);
    expect(niceStep(2, 0)).toBe(2);
  });
});

describe('computeProfileGeometry', () => {
  const flat = (lengthM?: number) => ({
    lengthM,
    points: [
      { distanceM: 2000, groundMslM: 130 },
      { distanceM: 0, groundMslM: 100 },
      { distanceM: 1000, groundMslM: 110 },
    ],
  });

  it('takes the length and the far end ground from the last point when the profile has no length', () => {
    const g = computeProfileGeometry({ profile: flat(), fromAglM: 2, toAglM: 50, frequencyHz: 915e6 });
    expect(g.lengthM).toBe(2000);
    expect(g.fromMsl).toBe(102);
    expect(g.toMsl).toBe(180);
    expect(g.samples.map((s) => s.d)).toEqual([0, 1000, 2000]);
    expect(g.intrusions).toEqual([]);
  });

  it('prefers the stated length and antenna heights above sea level', () => {
    const g = computeProfileGeometry({
      profile: flat(2500),
      fromAglM: 2,
      toAglM: 50,
      fromAntennaMslM: 300,
      toAntennaMslM: 400,
      frequencyHz: 915e6,
    });
    expect(g.lengthM).toBe(2500);
    expect([g.fromMsl, g.toMsl]).toEqual([300, 400]);
  });

  it('reports a hill that enters the first Fresnel zone as an intrusion', () => {
    const profile = {
      points: [
        { distanceM: 0, groundMslM: 100 },
        { distanceM: 1000, groundMslM: 200 },
        { distanceM: 2000, groundMslM: 100 },
      ],
    };
    const g = computeProfileGeometry({ profile, fromAglM: 2, toAglM: 2, frequencyHz: 915e6 });
    expect(g.intrusions).toHaveLength(1);
    expect(g.intrusions[0].maxD).toBe(1000);
    expect(g.intrusions[0].clearance).toBeLessThan(0);
    expect(g.worst.d).toBe(1000);
  });

  it('needs two points', () => {
    expect(() =>
      computeProfileGeometry({ profile: { points: [{ distanceM: 0, groundMslM: 1 }] }, fromAglM: 1, toAglM: 1, frequencyHz: 1e9 }),
    ).toThrow(/two points/);
  });

  it('has no Fresnel radius at the ends', () => {
    expect(fresnelRadius(0.3, 0, 100)).toBe(0);
    expect(fresnelRadius(0.3, 50, 50)).toBeCloseTo(Math.sqrt(0.3 * 25), 9);
  });
});
