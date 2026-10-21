import type { FeatureCollection, LineString } from 'geojson';
import type { GeoJSONSource, LayerSpecification, SourceSpecification } from 'maplibre-gl';
import { StyleLayerSet } from '../layers/layer-definition';
import { LatLon } from '../tools/geo';

/** One coloured piece of the route in weather mode. */
export interface WeatherRibbonSegment {
  from: LatLon;
  to: LatLon;
  color: string;
}

/**
 * The route in weather mode: the same single line per leg, recoloured by the chosen weather value
 * at the time the drone is expected there. It is drawn right over the route line, on a solid
 * casing, so the radio colours and dashes under it do not show through. Nothing is drawn outside
 * weather mode.
 */
export class RouteWeatherLayer extends StyleLayerSet {
  readonly id = 'mission-route-weather';
  readonly title = 'Route weather';
  readonly group = 'mission' as const;
  override readonly order = 1;
  override readonly internal = true;
  private data: FeatureCollection<LineString> = { type: 'FeatureCollection', features: [] };

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-route-weather': { type: 'geojson', data: this.data } };
  }

  protected layers(): LayerSpecification[] {
    const layout = { 'line-cap': 'round', 'line-join': 'round' } as const;
    return [
      { id: 'route-weather-casing', type: 'line', source: 'rf-route-weather', layout, paint: { 'line-color': '#0B0F14', 'line-width': 10 } },
      { id: 'route-weather', type: 'line', source: 'rf-route-weather', layout, paint: { 'line-color': ['get', 'color'], 'line-width': 4 } },
    ];
  }

  setSegments(segments: readonly WeatherRibbonSegment[]): void {
    this.data = {
      type: 'FeatureCollection',
      features: segments.map((s) => ({
        type: 'Feature',
        properties: { color: s.color },
        geometry: { type: 'LineString', coordinates: [[s.from.lon, s.from.lat], [s.to.lon, s.to.lat]] },
      })),
    };
    (this.map?.getSource('rf-route-weather') as GeoJSONSource | undefined)
      ?.setData(this.data)
      .catch((e: unknown) => console.error('Route weather could not be updated', e));
  }
}

/** A point in screen pixels. */
export interface ScreenPoint {
  x: number;
  y: number;
}

/** Where a cursor is nearest to a polyline drawn on the screen. */
export interface PolylineHit {
  /** Index of the segment from vertex `segment` to `segment + 1` */
  segment: number;
  /** Position along that segment, 0..1 */
  t: number;
  distancePx: number;
}

/** Finds the place on a projected polyline nearest to a cursor, for hover read-outs. */
export class PolylineHitTest {
  constructor(private readonly vertices: readonly ScreenPoint[]) {}

  /** The nearest place within `tolerancePx`, or null when the cursor is farther from the line. */
  nearest(cursor: ScreenPoint, tolerancePx: number): PolylineHit | null {
    let best: PolylineHit | null = null;
    for (let i = 0; i + 1 < this.vertices.length; i++) {
      const hit = this.onSegment(i, cursor);
      if (hit.distancePx <= tolerancePx && (!best || hit.distancePx < best.distancePx)) best = hit;
    }
    return best;
  }

  private onSegment(i: number, c: ScreenPoint): PolylineHit {
    const a = this.vertices[i];
    const b = this.vertices[i + 1];
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const len2 = dx * dx + dy * dy;
    const t = len2 > 0 ? Math.min(1, Math.max(0, ((c.x - a.x) * dx + (c.y - a.y) * dy) / len2)) : 0;
    const px = a.x + dx * t;
    const py = a.y + dy * t;
    return { segment: i, t, distancePx: Math.hypot(c.x - px, c.y - py) };
  }
}
