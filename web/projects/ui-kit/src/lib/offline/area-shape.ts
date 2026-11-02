import type { FeatureCollection, Polygon } from 'geojson';
import type { GeoJSONSource, LayerSpecification, SourceSpecification } from 'maplibre-gl';
import { StyleLayerSet } from '../layers/layer-definition';
import { EARTH_RADIUS_M, LatLon } from '../tools/geo';

/**
 * A polygon the user draws on the map, one corner per click, to ask the server for the offline
 * package of that area. Its area is measured as the server measures it (on the mean sphere), so
 * the limit shown while drawing is the limit the server applies.
 */
export class AreaShape {
  constructor(readonly points: readonly LatLon[] = []) {}

  get closed(): boolean {
    return this.points.length >= 3;
  }

  with(point: LatLon): AreaShape {
    return new AreaShape([...this.points, point]);
  }

  withoutLast(): AreaShape {
    return new AreaShape(this.points.slice(0, -1));
  }

  /** The area in km², 0 until there are three corners. */
  areaKm2(): number {
    const n = this.points.length;
    if (n < 3) return 0;
    const rad = Math.PI / 180;
    let sum = 0;
    for (let i = 0; i < n; i++) {
      const previous = this.points[(i + n - 1) % n];
      const next = this.points[(i + 1) % n];
      sum += (next.lon - previous.lon) * rad * Math.sin(this.points[i].lat * rad);
    }
    const r = EARTH_RADIUS_M / 1000;
    return (Math.abs(sum) * r * r) / 2;
  }

  /** The GeoJSON Polygon, closed; null until there are three corners. */
  geometry(): Polygon | null {
    if (!this.closed) return null;
    const ring = this.points.map((p) => [p.lon, p.lat]);
    return { type: 'Polygon', coordinates: [[...ring, ring[0]]] };
  }

  /** Whether two edges that do not share a corner cross: such a polygon has no clear inside. */
  crossesItself(): boolean {
    const n = this.points.length;
    if (n < 4) return false;
    const edge = (i: number): [LatLon, LatLon] => [this.points[i], this.points[(i + 1) % n]];
    for (let i = 0; i < n; i++) {
      for (let j = i + 2; j < n; j++) {
        if (i === 0 && j === n - 1) continue; // neighbours through the closing edge
        if (AreaShape.cross(edge(i), edge(j))) return true;
      }
    }
    return false;
  }

  private static cross([a, b]: [LatLon, LatLon], [c, d]: [LatLon, LatLon]): boolean {
    const side = (p: LatLon, q: LatLon, r: LatLon) => Math.sign((q.lon - p.lon) * (r.lat - p.lat) - (q.lat - p.lat) * (r.lon - p.lon));
    return side(a, b, c) * side(a, b, d) < 0 && side(c, d, a) * side(c, d, b) < 0;
  }
}

/** The polygon being drawn: its fill, outline and corners. */
export class AreaShapeLayer extends StyleLayerSet {
  readonly id = 'offline-area-shape';
  readonly title = 'Drawn area';
  readonly group = 'mission' as const;
  override readonly internal = true;
  private data: FeatureCollection = { type: 'FeatureCollection', features: [] };

  show(shape: AreaShape, over: boolean): void {
    const corners = shape.points.map((p) => ({
      type: 'Feature' as const,
      properties: { kind: 'corner' },
      geometry: { type: 'Point' as const, coordinates: [p.lon, p.lat] },
    }));
    const geometry = shape.geometry();
    const outline = shape.points.length >= 2 ? [{
      type: 'Feature' as const,
      properties: { kind: geometry ? 'area' : 'line', over },
      geometry: geometry ?? { type: 'LineString' as const, coordinates: shape.points.map((p) => [p.lon, p.lat]) },
    }] : [];
    this.data = { type: 'FeatureCollection', features: [...outline, ...corners] };
    (this.map?.getSource('rf-offline-area') as GeoJSONSource | undefined)?.setData(this.data);
  }

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-offline-area': { type: 'geojson', data: this.data } };
  }

  protected layers(): LayerSpecification[] {
    const color = ['case', ['get', 'over'], '#FF6B5B', '#35D0BA'] as never;
    return [
      { id: 'offline-area-fill', type: 'fill', source: 'rf-offline-area', filter: ['==', ['get', 'kind'], 'area'], paint: { 'fill-color': color, 'fill-opacity': 0.18 } },
      { id: 'offline-area-line', type: 'line', source: 'rf-offline-area', filter: ['!=', ['get', 'kind'], 'corner'], paint: { 'line-color': color, 'line-width': 2.5 } },
      { id: 'offline-area-corner', type: 'circle', source: 'rf-offline-area', filter: ['==', ['get', 'kind'], 'corner'], paint: { 'circle-radius': 5, 'circle-color': '#0B0F14', 'circle-stroke-color': '#7BE6D6', 'circle-stroke-width': 2 } },
    ];
  }
}
