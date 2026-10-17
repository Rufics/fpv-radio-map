import { describe, expect, it } from 'vitest';
import { CoverageGrid } from '../models/api.models';
import { DeltaScale, sampleGrid, subtractGrids } from './delta';

const grid = (margins: (number | null)[], bbox: [number, number, number, number] = [35, 48, 35.2, 48.2]): CoverageGrid => ({ cols: 2, rows: 2, bbox, marginDb: margins });

describe('coverage delta', () => {
  it('subtracts aligned grids cell by cell (B − A)', () => {
    const d = subtractGrids(grid([10, 5, null, -2]), grid([13, 4, 7, -2]));
    expect(d?.marginDb).toEqual([3, -1, null, 0]);
  });

  it('samples B at A cell centres when the grids differ', () => {
    const b: CoverageGrid = { cols: 1, rows: 1, bbox: [34, 47, 36, 49], marginDb: [20] };
    const d = subtractGrids(grid([10, 5, 0, 1]), b);
    expect(d?.marginDb).toEqual([10, 15, 20, 19]);
    expect(sampleGrid(b, 40, 48)).toBeNull();
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
