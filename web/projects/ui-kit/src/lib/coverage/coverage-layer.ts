import type { Feature, FeatureCollection, Geometry, Position } from 'geojson';
import type { GeoJSONSource, ImageSource, LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import { CoverageGrid } from '../models/api.models';
import { StyleLayerSet } from '../layers/layer-definition';
import { HeatScale } from './heat-scale';
import { destinationPoint } from '../tools/geo';

/** The heatmap's opacity on the flat map, and draped over raised terrain under the relief shade. */
const OPACITY_FLAT = 0.55;
const OPACITY_DRAPED = 0.38;
/** Draped, the colours are muted a little so the shading, not the hue, carries the shape. */
const SATURATION_DRAPED = -0.35;
const RELIEF_SOURCE = 'rf-coverage-relief';
const RELIEF_LAYER = 'coverage-relief';

const EMPTY_PNG =
  'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=';

/** [west, south, east, north] of a grid; from bbox, or derived from origin (north-west corner) and cell size. */
export function gridBounds(grid: CoverageGrid): [number, number, number, number] | null {
  if (grid.bbox?.length === 4) return grid.bbox;
  const o = grid.origin;
  if (!o?.lat || !o.lon || !grid.cellM || !grid.cols || !grid.rows) return null;
  const east = destinationPoint(o.lat, o.lon, 90, grid.cellM * grid.cols);
  const south = destinationPoint(o.lat, o.lon, 180, grid.cellM * grid.rows);
  return [o.lon, south.lat, east.lon, o.lat];
}

/** Margin grid → heatmap with the 4-colour scale; below threshold is transparent. */
export class CoverageLayer extends StyleLayerSet {
  readonly id = 'results-coverage';
  readonly title = 'Coverage heatmap';
  readonly group = 'results' as const;
  override readonly legend = 'Signal margin, strong to threshold';
  private readonly scale = new HeatScale(0, 255);
  private url = EMPTY_PNG;
  private coords: [[number, number], [number, number], [number, number], [number, number]] = [
    [0, 0.0001],
    [0.0001, 0.0001],
    [0.0001, 0],
    [0, 0],
  ];
  private hasGrid = false;
  private visible = true;
  private relief: SourceSpecification | null = null;

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-coverage': { type: 'image', url: this.url, coordinates: this.coords } };
  }

  protected layers(): LayerSpecification[] {
    return [
      {
        id: 'coverage-heat',
        type: 'raster',
        source: 'rf-coverage',
        paint: { 'raster-opacity': OPACITY_FLAT, 'raster-resampling': 'linear', 'raster-fade-duration': 0 },
      },
    ];
  }

  /** Render a grid; null clears the layer. */
  setGrid(grid: CoverageGrid | null): void {
    if (!grid?.marginDb || !grid.cols || !grid.rows) {
      this.hasGrid = false;
      this.url = EMPTY_PNG;
    } else {
      const bounds = gridBounds(grid);
      if (!bounds) return;
      const [w, s, e, n] = bounds;
      this.coords = [
        [w, n],
        [e, n],
        [e, s],
        [w, s],
      ];
      this.url = this.render(grid);
      this.hasGrid = true;
    }
    const src = this.map?.getSource('rf-coverage') as ImageSource | undefined;
    src?.updateImage({ url: this.url, coordinates: this.coords });
    if (this.map) this.setVisible(this.map, this.visible);
  }

  /**
   * 3D: the heatmap is draped over raised terrain. It is drawn lighter, and a relief shade from
   * `dem` goes right over it, so slopes and valleys read through the colours. null is the flat map.
   */
  setRelief(dem: SourceSpecification | null): void {
    this.relief = dem;
    if (this.map) this.applyRelief(this.map);
  }

  override attach(map: MlMap, beforeId: string | undefined): void {
    super.attach(map, beforeId);
    this.applyRelief(map);
  }

  override detach(map: MlMap): void {
    this.removeRelief(map);
    super.detach(map);
  }

  override setVisible(map: MlMap, visible: boolean): void {
    this.visible = visible;
    super.setVisible(map, visible && this.hasGrid);
    if (map.getLayer(RELIEF_LAYER)) map.setLayoutProperty(RELIEF_LAYER, 'visibility', visible && this.hasGrid ? 'visible' : 'none');
  }

  private applyRelief(map: MlMap): void {
    if (!map.getLayer('coverage-heat')) return;
    map.setPaintProperty('coverage-heat', 'raster-opacity', this.relief ? OPACITY_DRAPED : OPACITY_FLAT);
    map.setPaintProperty('coverage-heat', 'raster-saturation', this.relief ? SATURATION_DRAPED : 0);
    if (!this.relief) return this.removeRelief(map);
    if (!map.getSource(RELIEF_SOURCE)) map.addSource(RELIEF_SOURCE, this.relief);
    if (!map.getLayer(RELIEF_LAYER)) {
      const order = map.getLayersOrder();
      const above = order[order.indexOf('coverage-heat') + 1];
      map.addLayer(
        {
          id: RELIEF_LAYER,
          type: 'hillshade',
          source: RELIEF_SOURCE,
          paint: {
            // shadows darken the colours, sunlit slopes lift them a little; flat ground is left as is
            'hillshade-shadow-color': 'rgba(4, 7, 11, 0.75)',
            'hillshade-highlight-color': 'rgba(255, 255, 255, 0.25)',
            'hillshade-accent-color': 'rgba(4, 7, 11, 0.3)',
            'hillshade-exaggeration': 0.9,
            'hillshade-illumination-anchor': 'map',
          },
        },
        above,
      );
    }
    this.setVisible(map, this.visible);
  }

  private removeRelief(map: MlMap): void {
    if (map.getLayer(RELIEF_LAYER)) map.removeLayer(RELIEF_LAYER);
    if (map.getSource(RELIEF_SOURCE)) map.removeSource(RELIEF_SOURCE);
  }

  private render(grid: CoverageGrid): string {
    const cols = grid.cols ?? 0;
    const rows = grid.rows ?? 0;
    const canvas = document.createElement('canvas');
    canvas.width = cols;
    canvas.height = rows;
    const ctx = canvas.getContext('2d');
    if (!ctx) return EMPTY_PNG;
    const img = ctx.createImageData(cols, rows);
    this.scale.paint(grid.marginDb ?? [], img.data);
    ctx.putImageData(img, 0, 0);
    return canvas.toDataURL('image/png');
  }
}

export interface LimitShapes {
  /** 0 dB line of the link shown in the heatmap */
  radio?: Geometry | Feature | null;
  radioLabel?: string;
  /** Out-and-back battery limit around the pilot */
  battery?: { lat: number; lon: number; radiusM: number; label?: string } | null;
}

/** Radio limit (0 dB margin line) and battery return limit. */
export class LimitsLayer extends StyleLayerSet {
  readonly id = 'results-limits';
  readonly title = 'Radio and battery limits';
  readonly group = 'results' as const;
  override readonly order = 1;
  override readonly legend = 'Solid: radio limit · dashed: battery return limit';

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-limits': { type: 'geojson', data: { type: 'FeatureCollection', features: [] } } };
  }

  protected layers(): LayerSpecification[] {
    return [
      {
        id: 'limits-radio',
        type: 'line',
        source: 'rf-limits',
        filter: ['==', ['get', 'kind'], 'radio'],
        paint: { 'line-color': '#35D0BA', 'line-width': 2.5 },
      },
      {
        id: 'limits-battery',
        type: 'line',
        source: 'rf-limits',
        filter: ['==', ['get', 'kind'], 'battery'],
        paint: { 'line-color': '#F5A524', 'line-width': 2, 'line-dasharray': [5, 3] },
      },
    ];
  }

  setShapes(shapes: LimitShapes): void {
    const features: Feature[] = [];
    if (shapes.radio) {
      const geom = 'type' in shapes.radio && shapes.radio.type === 'Feature' ? (shapes.radio as Feature).geometry : (shapes.radio as Geometry);
      if (geom) features.push({ type: 'Feature', properties: { kind: 'radio', label: shapes.radioLabel ?? '' }, geometry: toLines(geom) });
    }
    if (shapes.battery) {
      const ring: Position[] = [];
      for (let b = 0; b <= 360; b += 4) {
        const p = destinationPoint(shapes.battery.lat, shapes.battery.lon, b, shapes.battery.radiusM);
        ring.push([p.lon, p.lat]);
      }
      features.push({ type: 'Feature', properties: { kind: 'battery', label: shapes.battery.label ?? '' }, geometry: { type: 'LineString', coordinates: ring } });
    }
    const fc: FeatureCollection = { type: 'FeatureCollection', features };
    this.pushData(this.map, fc);
    this.pending = fc;
  }

  private pending?: FeatureCollection;

  override attach(map: MlMap, beforeId: string | undefined): void {
    super.attach(map, beforeId);
    if (this.pending) this.pushData(map, this.pending);
  }

  private pushData(map: MlMap | undefined, fc: FeatureCollection): void {
    (map?.getSource('rf-limits') as GeoJSONSource | undefined)
      ?.setData(fc)
      .catch((e: unknown) => console.error('Limit shapes could not be updated', e));
  }
}

function toLines(geom: Geometry): Geometry {
  if (geom.type === 'Polygon') return { type: 'MultiLineString', coordinates: geom.coordinates };
  if (geom.type === 'MultiPolygon') return { type: 'MultiLineString', coordinates: geom.coordinates.flat() };
  return geom;
}
