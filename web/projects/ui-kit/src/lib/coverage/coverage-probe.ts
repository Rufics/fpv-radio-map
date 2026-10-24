import { CoverageGrid } from '../models/api.models';
import { LatLon } from '../tools/geo';
import { gridBounds } from './coverage-layer';

/**
 * Reads a coverage grid at a place: the margin of the cell under a point, and where a point falls
 * on the grid image. The image is stretched between its corners in Web Mercator (as MapLibre
 * places an image source), so rows are spaced in Mercator y, not in latitude.
 */
export class CoverageProbe {
  private constructor(
    private readonly grid: CoverageGrid,
    readonly bounds: [number, number, number, number],
  ) {}

  /** A probe for a grid with margins, or null. */
  static of(grid: CoverageGrid | null | undefined): CoverageProbe | null {
    if (!grid?.marginDb || !grid.cols || !grid.rows) return null;
    const bounds = gridBounds(grid);
    return bounds ? new CoverageProbe(grid, bounds) : null;
  }

  /** Web Mercator y of a latitude, unscaled (grows to the north). */
  static mercatorY(latDeg: number): number {
    const phi = (latDeg * Math.PI) / 180;
    return Math.log(Math.tan(Math.PI / 4 + phi / 2));
  }

  get cols(): number {
    return this.grid.cols ?? 0;
  }

  get rows(): number {
    return this.grid.rows ?? 0;
  }

  /** Position of a point on the grid image, 0..1 from the west and from the north edge. */
  fraction(p: LatLon): { x: number; y: number } {
    const [w, s, e, n] = this.bounds;
    const top = CoverageProbe.mercatorY(n);
    return {
      x: (p.lon - w) / (e - w),
      y: (top - CoverageProbe.mercatorY(p.lat)) / (top - CoverageProbe.mercatorY(s)),
    };
  }

  /** The point at a position on the grid image (the inverse of `fraction`). */
  pointAt(x: number, y: number): LatLon {
    const [w, s, e, n] = this.bounds;
    const top = CoverageProbe.mercatorY(n);
    const my = top - y * (top - CoverageProbe.mercatorY(s));
    return { lat: (Math.atan(Math.sinh(my)) * 180) / Math.PI, lon: w + x * (e - w) };
  }

  /** Margin of the cell under a point in dB; null outside the grid or where the cell has no value. */
  marginAt(p: LatLon): number | null {
    const { x, y } = this.fraction(p);
    if (x < 0 || y < 0 || x >= 1 || y >= 1) return null;
    const col = Math.floor(x * this.cols);
    const row = Math.floor(y * this.rows);
    const v = this.grid.marginDb?.[row * this.cols + col];
    return v === undefined || v === null || Number.isNaN(v) ? null : v;
  }
}
