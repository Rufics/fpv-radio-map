import { describe, expect, it } from 'vitest';
import { CoverageGrid } from '../models/api.models';
import { DeltaScale, sampleGrid, subtractGrids } from './delta';

// 2 × 2 cells of 0.1° from the north-west corner (35.0, 48.2)
const grid = (margins: (number | null)[], origin = { lat: 48.2, lon: 35.0 }, cols = 2, rows = 2): CoverageGrid => ({
  cols,
  rows,
  origin,
  cellLatDeg: 0.1,
  cellLonDeg: 0.1,
  bbox: [origin.lon, origin.lat - rows * 0.1, origin.lon + cols * 0.1, origin.lat],
  marginDb: margins,
});

describe('coverage difference B − A (exact cells only)', () => {
  it('subtracts identical grids cell by cell', () => {
    const d = subtractGrids(grid([10, 5, null, -2]), grid([13, 4, 7, -2]))!;
    expect(d.alignment).toBe('same');
    expect(d.grid.marginDb).toEqual([3, -1, null, 0]);
    expect(d.notComparableCells).toBe(0);
  });

  it('matches cells of a grid offset by whole cells, the rest is not comparable', () => {
    // B starts one column further east: B[c] coincides with A[c + 1]
    const b = grid([20, 21, 22, 23], { lat: 48.2, lon: 35.1 });
    const d = subtractGrids(grid([10, 5, 0, 1]), b)!;
    expect(d.alignment).toBe('shifted');
    expect(d.comparable).toEqual([false, true, false, true]);
    expect(d.grid.marginDb).toEqual([null, 20 - 5, null, 22 - 1]);
    expect(d.notComparableCells).toBe(2);
  });

  it('never resamples: a grid with other cells is not comparable anywhere', () => {
    const b: CoverageGrid = { ...grid([20, 20, 20, 20], { lat: 48.25, lon: 35.0 }) };
    const d = subtractGrids(grid([10, 5, 0, 1]), b)!;
    expect(d.alignment).toBe('none');
    expect(d.comparable.every((c) => !c)).toBe(true);
    expect(d.grid.marginDb!.every((v) => v === null)).toBe(true);
    const other = subtractGrids(grid([1, 1, 1, 1]), { ...grid([2, 2, 2, 2]), cellLonDeg: 0.11 })!;
    expect(other.alignment).toBe('none');
  });

  it('samples the cell containing a point, null outside', () => {
    const g = grid([1, 2, 3, 4]);
    expect(sampleGrid(g, 35.15, 48.15)).toBe(2);
    expect(sampleGrid(g, 40, 48)).toBeNull();
  });

  it('marks direction with text as well as colour', () => {
    const s = new DeltaScale(1);
    expect(s.mark(3)).toBe('▲');
    expect(s.mark(-3)).toBe('▼');
    expect(s.mark(0.4)).toBe('=');
    expect(s.color(0.5)).toBeNull();
    expect(s.color(5)?.slice(0, 3)).toEqual([...DeltaScale.BETTER]);
  });
});
