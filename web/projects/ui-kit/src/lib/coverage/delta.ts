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

/** Where a grid cell (row, col) lies, and the reverse lookup. */
function cellCenter(grid: CoverageGrid, bounds: [number, number, number, number], r: number, c: number): [number, number] {
  const [w, s, e, n] = bounds;
  const cols = grid.cols ?? 1;
  const rows = grid.rows ?? 1;
  return [w + ((c + 0.5) / cols) * (e - w), n - ((r + 0.5) / rows) * (n - s)];
}

/** Margin of `grid` at lon/lat (nearest cell), or null outside / no data. */
export function sampleGrid(grid: CoverageGrid, lon: number, lat: number): number | null {
  const b = gridBounds(grid);
  if (!b || !grid.marginDb || !grid.cols || !grid.rows) return null;
  const [w, s, e, n] = b;
  if (lon < w || lon > e || lat < s || lat > n) return null;
  const c = Math.min(grid.cols - 1, Math.floor(((lon - w) / (e - w)) * grid.cols));
  const r = Math.min(grid.rows - 1, Math.floor(((n - lat) / (n - s)) * grid.rows));
  return grid.marginDb[r * grid.cols + c] ?? null;
}

/**
 * B − A on A's cells. When both grids have the same geometry this is a cell-by-cell subtraction;
 * otherwise B is sampled at A's cell centres (nearest cell). A cell is null when either side has no value.
 * A cell where one side is below threshold still counts: the margins are the server's numbers either way.
 */
export function subtractGrids(a: CoverageGrid, b: CoverageGrid): CoverageGrid | null {
  const ba = gridBounds(a);
  if (!ba || !a.marginDb || !b.marginDb || !a.cols || !a.rows) return null;
  const bb = gridBounds(b);
  const same = !!bb && a.cols === b.cols && a.rows === b.rows && ba.every((v, i) => Math.abs(v - bb[i]) < 1e-9);
  const out: (number | null)[] = new Array(a.cols * a.rows);
  for (let r = 0; r < a.rows; r++) {
    for (let c = 0; c < a.cols; c++) {
      const i = r * a.cols + c;
      const va = a.marginDb[i];
      let vb: number | null | undefined;
      if (same) vb = b.marginDb[i];
      else {
        const [lon, lat] = cellCenter(a, ba, r, c);
        vb = sampleGrid(b, lon, lat);
      }
      out[i] = va === null || va === undefined || vb === null || vb === undefined ? null : vb - va;
    }
  }
  return { ...a, marginDb: out, limitPolygon: undefined };
}

/** Heatmap of a B − A grid with the diverging DeltaScale. */
export class CoverageDeltaLayer extends StyleLayerSet {
  readonly id = 'results-coverage-delta';
  readonly title = 'Coverage difference, B − A';
  readonly group = 'results' as const;
  override readonly order = 0.5;
  override readonly legend = '▲ B better · ▼ B worse · under 1 dB not shown';
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

  /** Show a delta grid (from subtractGrids); null clears. */
  setDelta(delta: CoverageGrid | null): void {
    const b = delta ? gridBounds(delta) : null;
    if (!delta || !b || !delta.marginDb) {
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

  private render(grid: CoverageGrid): string {
    const cols = grid.cols ?? 0;
    const rows = grid.rows ?? 0;
    const canvas = document.createElement('canvas');
    canvas.width = cols;
    canvas.height = rows;
    const ctx = canvas.getContext('2d');
    if (!ctx) return this.url;
    const img = ctx.createImageData(cols, rows);
    const m = grid.marginDb ?? [];
    for (let i = 0; i < m.length; i++) {
      const c = this.scale.color(m[i]);
      if (c) img.data.set(c, i * 4);
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
