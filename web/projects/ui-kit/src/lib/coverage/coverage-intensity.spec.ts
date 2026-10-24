import { describe, expect, it } from 'vitest';
import { CoverageIntensity } from './coverage-intensity';

const idle = { flight: false, miniMap: false, dragging: false, editing: false };

describe('CoverageIntensity', () => {
  it('draws the heatmap fully while a result is read', () => {
    expect(CoverageIntensity.activity(idle)).toBe('analyse');
    expect(CoverageIntensity.opacity('analyse')).toBeGreaterThan(0.4);
    expect(CoverageIntensity.note('analyse')).toBeNull();
  });

  it('fades it while editing and more while a point is dragged', () => {
    const edit = CoverageIntensity.activity({ ...idle, editing: true });
    const drag = CoverageIntensity.activity({ ...idle, editing: true, dragging: true });
    expect([edit, drag]).toEqual(['edit', 'drag']);
    expect(CoverageIntensity.opacity(drag)).toBeLessThan(CoverageIntensity.opacity(edit));
    expect(CoverageIntensity.opacity(edit)).toBeLessThan(CoverageIntensity.opacity('analyse'));
    expect(CoverageIntensity.note(edit)).toMatch(/edit/);
  });

  it('turns it off for a flight replay before anything else, and for the mini-map', () => {
    expect(
      CoverageIntensity.activity({ flight: true, miniMap: true, dragging: true, editing: true }),
    ).toBe('flight');
    expect(CoverageIntensity.activity({ ...idle, miniMap: true, editing: true })).toBe('mini-map');
    expect(CoverageIntensity.opacity('flight')).toBe(0);
    expect(CoverageIntensity.opacity('mini-map')).toBe(0);
  });
});
