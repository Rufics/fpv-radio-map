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
    const rgb = Math.abs(deltaDb) < this.deadBandDb ? DeltaScale.SAME : deltaDb > 0 ? DeltaScale.BETTER : DeltaScale.WORSE;
    return `rgb(${rgb.join(',')})`;
  }

  /** Text mark so the direction never depends on colour alone. */
  mark(deltaDb: number): '▲' | '▼' | '=' {
    return Math.abs(deltaDb) < this.deadBandDb ? '=' : deltaDb > 0 ? '▲' : '▼';
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
  const ca = cellDeg(a);
  const cb = cellDeg(b);
  const oa = northWest(a);
  const ob = northWest(b);
  let alignment: CoverageDelta['alignment'] = 'none';
  let dr = 0;
  let dc = 0;
  if (ca && cb && oa && ob && Math.abs(ca.dLat - cb.dLat) <= EPS * ca.dLat && Math.abs(ca.dLon - cb.dLon) <= EPS * ca.dLon) {
    const rows = (ob.lat - oa.lat) / ca.dLat; // B starts this many rows north of A
    const cols = (oa.lon - ob.lon) / ca.dLon; // B starts this many cols west of A
    if (Math.abs(rows - Math.round(rows)) < 1e-6 && Math.abs(cols - Math.round(cols)) < 1e-6) {
      dr = Math.round(rows);
      dc = Math.round(cols);
      alignment = dr === 0 && dc === 0 && a.cols === b.cols && a.rows === b.rows ? 'same' : 'shifted';
    }
  }
  if (alignment !== 'none') {
    for (let r = 0; r < a.rows; r++) {
      const rb = r + dr;
      if (rb < 0 || rb >= b.rows) continue;
      for (let c = 0; c < a.cols; c++) {
        const cbI = c + dc;
        if (cbI < 0 || cbI >= b.cols) continue;
        const i = r * a.cols + c;
        comparable[i] = true;
        const va = a.marginDb[i];
        const vb = b.marginDb[rb * b.cols + cbI];
        out[i] = va === null || va === undefined || vb === null || vb === undefined ? null : vb - va;
      }
    }
  }
  const notComparableCells = comparable.reduce((k, ok) => k + (ok ? 0 : 1), 0);
  return { grid: { ...a, marginDb: out, marginWorstDb: undefined, limitPolygon: undefined }, comparable, notComparableCells, alignment };
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
    const m = grid.marginDb ?? [];
    const hatch: [number, number, number, number] = [...DeltaScale.SAME, 150];
    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < cols; c++) {
        const i = r * cols + c;
        const col = delta.comparable[i] ? this.scale.color(m[i]) : null;
        for (let y = 0; y < s; y++) {
          for (let x = 0; x < s; x++) {
            const px = ((r * s + y) * cols * s + (c * s + x)) * 4;
            if (col) img.data.set(col, px);
            else if (!delta.comparable[i] && ((r * s + y + c * s + x) % 4 === 0)) img.data.set(hatch, px);
          }
        }
      }
    }
    ctx.putImageData(img, 0, 0);
    return canvas.toDataURL('image/png');
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
    (this.map?.getSource('rf-route-delta') as GeoJSONSource | undefined)?.setData(this.data);
  }
}
