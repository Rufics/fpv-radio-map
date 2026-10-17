import { describe, expect, it } from 'vitest';
import { groundToAir } from './link-view';

describe('groundToAir', () => {
  const profile = { lengthM: 1000, points: [{ distanceM: 0, groundMslM: 300 }, { distanceM: 400, groundMslM: 500 }, { distanceM: 1000, groundMslM: 100 }] };

  it('flips a profile ordered from the drone (video) so the pilot is on the left', () => {
    const v = groundToAir({ profile, ends: { transmitter: 'air' }, diffraction: { d1M: 400, d2M: 600, dominant: { distanceM: 400, groundMslM: 500 } } });
    expect(v.flipped).toBe(true);
    expect(v.profile?.points.map((p) => [p.distanceM, p.groundMslM])).toEqual([[0, 100], [600, 500], [1000, 300]]);
    expect(v.diffraction?.d1M).toBe(600);
    expect(v.diffraction?.dominant?.distanceM).toBe(600);
  });

  it('keeps a profile ordered from the ground (control)', () => {
    const v = groundToAir({ profile, ends: { transmitter: 'ground' } });
    expect(v.flipped).toBe(false);
    expect(v.profile).toBe(profile);
  });
});
