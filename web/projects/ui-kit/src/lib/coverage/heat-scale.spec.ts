import { describe, expect, it } from 'vitest';
import { HEAT_STOPS, HeatScale } from './heat-scale';

describe('HeatScale', () => {
  const scale = new HeatScale(0, 200);

  it('is transparent below the threshold and without data', () => {
    expect(scale.color(-0.1)).toBeNull();
    expect(scale.color(null)).toBeNull();
    expect(scale.color(Number.NaN)).toBeNull();
    expect(new HeatScale(5).color(4)).toBeNull();
  });

  it('clamps strong margins to the top stop', () => {
    expect(scale.color(30)).toEqual([0xf9, 0xe4, 0x6b, 200]);
    expect(scale.color(80)).toEqual([0xf9, 0xe4, 0x6b, 200]);
  });

  it('interpolates between two stops', () => {
    expect(scale.color(0)).toEqual([...HEAT_STOPS[0].rgb, 200]);
    const mid = scale.color(5)!;
    expect(mid[0]).toBe(Math.round((0x6a + 0xc9) / 2));
    expect(mid[3]).toBe(200);
  });

  it('paints coloured cells and clears the alpha of the others', () => {
    const target = new Uint8ClampedArray(8).fill(9);
    scale.paint([30, null], target);
    expect([...target]).toEqual([0xf9, 0xe4, 0x6b, 200, 9, 9, 9, 0]);
  });

  it('draws the legend gradient from strong to threshold', () => {
    expect(HeatScale.cssGradient()).toBe(
      'linear-gradient(90deg, rgb(249,228,107), rgb(242,141,60), rgb(201,65,122), rgb(106,44,143))',
    );
  });
});
