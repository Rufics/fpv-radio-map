import type { FeatureCollection, LineString } from 'geojson';
import type { GeoJSONSource, ImageSource, LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import { CoverageGrid } from '../models/api.models';
import { StyleLayerSet } from '../layers/layer-definition';
import { gridBounds } from './coverage-layer';

/**
 * Diverging scale for "scenario B minus scenario A", in dB.
 * B better: teal, B worse: coral; |Δ| below the dead band is transparent / grey.
 * Lightness grows with |Δ| so the two directions also read without colour; the legend adds ▲/▼ marks.
 */
export class DeltaScale {
  static readonly BETTER: [number, number, number] = [0x35, 0xd0, 0xba];
  static readonly WORSE: [number, number, number] = [0xff, 0x6b, 0x5b];
  static readonly SAME: [number, number, number] = [0x8a, 0x96, 0xa5];

  constructor(
    readonly deadBandDb = 1,
    readonly fullDb = 10,
  ) {}

  /** RGBA for a heatmap cell, null = transparent. */
  color(deltaDb: number | null | undefined): [number, number, number, number] | null {
    if (deltaDb === null || deltaDb === undefined || Number.isNaN(deltaDb)) return null;
    const a = Math.abs(deltaDb);
    if (a < this.deadBandDb) return null;
    const t = Math.min(1, (a - this.deadBandDb) / (this.fullDb - this.deadBandDb));
    const rgb = deltaDb > 0 ? DeltaScale.BETTER : DeltaScale.WORSE;
    return [rgb[0], rgb[1], rgb[2], Math.round(70 + 150 * t)];
  }

  /** CSS colour for a route segment. */
  css(deltaDb: number): string {
    const rgb = { same: DeltaScale.SAME, better: DeltaScale.BETTER, worse: DeltaScale.WORSE }[
      this.direction(deltaDb)
    ];
    return `rgb(${rgb.join(',')})`;
  }

  /** Text mark so the direction never depends on colour alone. */
  mark(deltaDb: number): '▲' | '▼' | '=' {
    const marks = { same: '=', better: '▲', worse: '▼' } as const;
    return marks[this.direction(deltaDb)];
  }

  /** Inside the dead band B and A count as the same. */
  private direction(deltaDb: number): 'same' | 'better' | 'worse' {
    if (Math.abs(deltaDb) < this.deadBandDb) return 'same';
    return deltaDb > 0 ? 'better' : 'worse';
  }
}

/** Cell size in degrees: the grid's cellLatDeg/cellLonDeg, else derived from its bbox. */
function cellDeg(g: CoverageGrid): { dLat: number; dLon: number } | null {
  if (g.cellLatDeg && g.cellLonDeg) return { dLat: g.cellLatDeg, dLon: g.cellLonDeg };
  const b = gridBounds(g);
  if (!b || !g.cols || !g.rows) return null;
  return { dLat: (b[3] - b[1]) / g.rows, dLon: (b[2] - b[0]) / g.cols };
}

function northWest(g: CoverageGrid): { lat: number; lon: number } | null {
  if (g.origin?.lat !== undefined && g.origin?.lon !== undefined) return { lat: g.origin.lat, lon: g.origin.lon };
  const b = gridBounds(g);
  return b ? { lat: b[3], lon: b[0] } : null;
}

/** Margin of `grid` in the cell that contains lon/lat, or null outside / no data. */
export function sampleGrid(grid: CoverageGrid, lon: number, lat: number): number | null {
  const b = gridBounds(grid);
  if (!b || !grid.marginDb || !grid.cols || !grid.rows) return null;
  const [w, s, e, n] = b;
  if (lon < w || lon > e || lat < s || lat > n) return null;
  const c = Math.min(grid.cols - 1, Math.floor(((lon - w) / (e - w)) * grid.cols));
  const r = Math.min(grid.rows - 1, Math.floor(((n - lat) / (n - s)) * grid.rows));
  return grid.marginDb[r * grid.cols + c] ?? null;
}

/** B − A on A's cells, with which cells could be compared at all. */
export interface CoverageDelta {
  /** A's geometry; marginDb holds B − A, null where a side has no value or the cell is not comparable */
  grid: CoverageGrid;
  /** per A cell: B has exactly the same cell */
  comparable: boolean[];
  notComparableCells: number;
  /** same = identical geometry; shifted = same cell size, origin offset by whole cells; none = nothing coincides */
  alignment: 'same' | 'shifted' | 'none';
}

const EPS = 1e-9;

/** How B's cells sit on A's: B cell = A cell shifted by dr rows and dc cols. */
interface GridOffset {
  alignment: CoverageDelta['alignment'];
  dr: number;
  dc: number;
}

/** Size and row-major margins of one grid side. */
interface GridCells {
  cols: number;
  rows: number;
  margin: (number | null)[];
}

const NOT_ALIGNED: GridOffset = { alignment: 'none', dr: 0, dc: 0 };

interface CellSize {
  dLat: number;
  dLon: number;
}

function sameCellSize(ca: CellSize, cb: CellSize): boolean {
  return Math.abs(ca.dLat - cb.dLat) <= EPS * ca.dLat && Math.abs(ca.dLon - cb.dLon) <= EPS * ca.dLon;
}

function isWholeNumber(v: number): boolean {
  return Math.abs(v - Math.round(v)) < 1e-6;
}

/** Offset of B's cells against A's, or NOT_ALIGNED when cell sizes or origins do not coincide. */
function gridOffset(a: CoverageGrid, b: CoverageGrid): GridOffset {
  const ca = cellDeg(a);
  const cb = cellDeg(b);
  const oa = northWest(a);
  const ob = northWest(b);
  if (!ca || !cb || !oa || !ob || !sameCellSize(ca, cb)) return NOT_ALIGNED;
  const rows = (ob.lat - oa.lat) / ca.dLat; // B starts this many rows north of A
  const cols = (oa.lon - ob.lon) / ca.dLon; // B starts this many cols west of A
  if (!isWholeNumber(rows) || !isWholeNumber(cols)) return NOT_ALIGNED;
  const dr = Math.round(rows);
  const dc = Math.round(cols);
  const same = dr === 0 && dc === 0 && a.cols === b.cols && a.rows === b.rows;
  return { alignment: same ? 'same' : 'shifted', dr, dc };
}

function cellDelta(va: number | null | undefined, vb: number | null | undefined): number | null {
  return va === null || va === undefined || vb === null || vb === undefined ? null : vb - va;
}

/** Marks every A cell that has a B cell at `offset` as comparable and writes B − A into `out`. */
function diffAlignedCells(
  a: GridCells,
  b: GridCells,
  offset: GridOffset,
  out: (number | null)[],
  comparable: boolean[],
): void {
  for (let r = 0; r < a.rows; r++) {
    const rb = r + offset.dr;
    if (rb < 0 || rb >= b.rows) continue;
    for (let c = 0; c < a.cols; c++) {
      const cbI = c + offset.dc;
      if (cbI < 0 || cbI >= b.cols) continue;
      const i = r * a.cols + c;
      comparable[i] = true;
      out[i] = cellDelta(a.margin[i], b.margin[rb * b.cols + cbI]);
    }
  }
}

/**
 * B − A where the cells coincide exactly, and nothing else: no resampling, no nearest cell.
 * Cells coincide when both grids use the same cell size in degrees and B's origin is offset from A's by a
 * whole number of cells (same pilot latitude and cell size, e.g. another radius or radio). Any other cell
 * is "not comparable". A cell where either side is null (beyond the radius, at the pilot, no DEM data)
 * is comparable but has no difference.
 */
export function subtractGrids(a: CoverageGrid, b: CoverageGrid): CoverageDelta | null {
  if (!a.marginDb || !b.marginDb || !a.cols || !a.rows || !b.cols || !b.rows) return null;
  const n = a.cols * a.rows;
  const out: (number | null)[] = new Array(n).fill(null);
  const comparable: boolean[] = new Array(n).fill(false);
  const offset = gridOffset(a, b);
  if (offset.alignment !== 'none') {
    const cellsA = { cols: a.cols, rows: a.rows, margin: a.marginDb };
    const cellsB = { cols: b.cols, rows: b.rows, margin: b.marginDb };
    diffAlignedCells(cellsA, cellsB, offset, out, comparable);
  }
  const notComparableCells = comparable.reduce((k, ok) => k + (ok ? 0 : 1), 0);
  return {
    grid: { ...a, marginDb: out, marginWorstDb: undefined, limitPolygon: undefined },
    comparable,
    notComparableCells,
    alignment: offset.alignment,
  };
}

/** RGBA pixels of a heatmap drawn with s × s pixels per grid cell. */
interface CellRaster {
  data: Uint8ClampedArray;
  cols: number;
  s: number;
}

/** Heatmap of a B − A grid with the diverging DeltaScale. */
export class CoverageDeltaLayer extends StyleLayerSet {
  readonly id = 'results-coverage-delta';
  readonly title = 'Coverage difference, B − A';
  readonly group = 'results' as const;
  override readonly order = 0.5;
  override readonly legend = '▲ B better · ▼ B worse · under 1 dB not shown · hatched: not comparable';
  private readonly scale = new DeltaScale();
  private url = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=';
  private coords: [[number, number], [number, number], [number, number], [number, number]] = [
    [0, 0.0001],
    [0.0001, 0.0001],
    [0.0001, 0],
    [0, 0],
  ];
  private has = false;
  private visible = true;

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-coverage-delta': { type: 'image', url: this.url, coordinates: this.coords } };
  }

  protected layers(): LayerSpecification[] {
    return [{ id: 'coverage-delta', type: 'raster', source: 'rf-coverage-delta', paint: { 'raster-opacity': 0.6, 'raster-resampling': 'nearest', 'raster-fade-duration': 0 } }];
  }

  /** Show a difference (from subtractGrids); null clears. Cells that are not comparable are hatched grey. */
  setDelta(delta: CoverageDelta | null): void {
    const b = delta ? gridBounds(delta.grid) : null;
    if (!delta || !b || !delta.grid.marginDb) {
      this.has = false;
    } else {
      const [w, s, e, n] = b;
      this.coords = [
        [w, n],
        [e, n],
        [e, s],
        [w, s],
      ];
      this.url = this.render(delta);
      this.has = true;
      (this.map?.getSource('rf-coverage-delta') as ImageSource | undefined)?.updateImage({ url: this.url, coordinates: this.coords });
    }
    if (this.map) super.setVisible(this.map, this.visible && this.has);
  }

  override attach(map: MlMap, beforeId: string | undefined): void {
    super.attach(map, beforeId);
    super.setVisible(map, this.visible && this.has);
  }

  override setVisible(map: MlMap, visible: boolean): void {
    this.visible = visible;
    super.setVisible(map, visible && this.has);
  }

  private render(delta: CoverageDelta): string {
    const grid = delta.grid;
    const cols = grid.cols ?? 0;
    const rows = grid.rows ?? 0;
    // 4 px per cell so the "not comparable" hatching stays visible
    const s = Math.max(cols, rows) > 600 ? 1 : 4;
    const canvas = document.createElement('canvas');
    canvas.width = cols * s;
    canvas.height = rows * s;
    const ctx = canvas.getContext('2d');
    if (!ctx) return this.url;
    const img = ctx.createImageData(cols * s, rows * s);
    this.paintCells(delta, { data: img.data, cols, s });
    ctx.putImageData(img, 0, 0);
    return canvas.toDataURL('image/png');
  }

  /** Comparable cells in the delta colour (none inside the dead band), the others hatched grey. */
  private paintCells(delta: CoverageDelta, raster: CellRaster): void {
    const rows = delta.grid.rows ?? 0;
    const m = delta.grid.marginDb ?? [];
    const hatch: [number, number, number, number] = [...DeltaScale.SAME, 150];
    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < raster.cols; c++) {
        const i = r * raster.cols + c;
        const col = delta.comparable[i] ? this.scale.color(m[i]) : hatch;
        if (col) this.paintCell(raster, r, c, col, !delta.comparable[i]);
      }
    }
  }

  /** Sets the s × s pixels of cell r/c to `rgba`; hatched sets only every 4th diagonal. */
  private paintCell(
    raster: CellRaster,
    r: number,
    c: number,
    rgba: number[],
    hatched: boolean,
  ): void {
    const { data, cols, s } = raster;
    for (let y = 0; y < s; y++) {
      for (let x = 0; x < s; x++) {
        if (hatched && (r * s + y + c * s + x) % 4 !== 0) continue;
        data.set(rgba, ((r * s + y) * cols * s + (c * s + x)) * 4);
      }
    }
  }
}

/** One compared point along the route (fpv.core compare output). */
export interface RouteDeltaPoint {
  lat: number;
  lon: number;
  deltaDb: number;
}

/** Colours the route by B − A: a wide casing under the route line, one segment per sample. */
export class RouteDeltaLayer extends StyleLayerSet {
  readonly id = 'results-route-delta';
  readonly title = 'Route difference, B − A';
  readonly group = 'results' as const;
  override readonly order = 3;
  override readonly legend = '▲ B better · ▼ B worse along the route';
  private readonly scale = new DeltaScale();
  private data: FeatureCollection<LineString> = { type: 'FeatureCollection', features: [] };

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-route-delta': { type: 'geojson', data: this.data } };
  }

  protected layers(): LayerSpecification[] {
    return [
      {
        id: 'route-delta',
        type: 'line',
        source: 'rf-route-delta',
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': ['get', 'color'], 'line-width': 12, 'line-opacity': 0.75 },
      },
    ];
  }

  setPoints(points: RouteDeltaPoint[] | null): void {
    const features: FeatureCollection<LineString>['features'] = [];
    const pts = points ?? [];
    for (let i = 0; i + 1 < pts.length; i++) {
      const d = (pts[i].deltaDb + pts[i + 1].deltaDb) / 2;
      features.push({
        type: 'Feature',
        properties: { color: this.scale.css(d), deltaDb: d },
        geometry: { type: 'LineString', coordinates: [[pts[i].lon, pts[i].lat], [pts[i + 1].lon, pts[i + 1].lat]] },
      });
    }
    this.data = { type: 'FeatureCollection', features };
    (this.map?.getSource('rf-route-delta') as GeoJSONSource | undefined)
      ?.setData(this.data)
      .catch((e: unknown) => console.error('Route difference could not be updated', e));
  }
}
