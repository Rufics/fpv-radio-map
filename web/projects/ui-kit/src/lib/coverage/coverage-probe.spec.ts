import { describe, expect, it } from 'vitest';
import { CoverageProbe } from './coverage-probe';

// 2 × 2 cells over [35.0, 48.0, 35.2, 48.2], north row first
const grid = {
  cols: 2,
  rows: 2,
  bbox: [35.0, 48.0, 35.2, 48.2] as [number, number, number, number],
  marginDb: [10, null, -3, 25],
};

describe('CoverageProbe', () => {
  it('is null for a missing grid or one without margins', () => {
    expect(CoverageProbe.of(null)).toBeNull();
    expect(CoverageProbe.of({ cols: 2, rows: 2 })).toBeNull();
  });

  it('reads the cell under a point, north row first', () => {
    const p = CoverageProbe.of(grid)!;
    expect(p.marginAt({ lat: 48.15, lon: 35.05 })).toBe(10);
    expect(p.marginAt({ lat: 48.05, lon: 35.05 })).toBe(-3);
    expect(p.marginAt({ lat: 48.05, lon: 35.15 })).toBe(25);
  });

  it('is null outside the grid and in a cell without a value', () => {
    const p = CoverageProbe.of(grid)!;
    expect(p.marginAt({ lat: 48.15, lon: 35.15 })).toBeNull();
    expect(p.marginAt({ lat: 48.3, lon: 35.1 })).toBeNull();
    expect(p.marginAt({ lat: 48.1, lon: 34.9 })).toBeNull();
  });

  it('places points in Web Mercator and inverts it', () => {
    const p = CoverageProbe.of(grid)!;
    expect(p.fraction({ lat: 48.2, lon: 35.0 })).toEqual({ x: 0, y: 0 });
    const mid = p.fraction({ lat: 48.1, lon: 35.1 });
    expect(mid.x).toBeCloseTo(0.5, 9);
    // Mercator stretches northwards, so the middle latitude sits a little below the middle
    expect(mid.y).toBeGreaterThan(0.5);
    const back = p.pointAt(mid.x, mid.y);
    expect(back.lat).toBeCloseTo(48.1, 9);
    expect(back.lon).toBeCloseTo(35.1, 9);
  });
});
