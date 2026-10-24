import type { Feature, FeatureCollection } from 'geojson';
import type { GeoJSONSource, LayerSpecification, SourceSpecification } from 'maplibre-gl';
import { StyleLayerSet } from '../layers/layer-definition';
import { LatLon } from '../tools/geo';

/** How a margin reads: a working link with room, one near its limit, none, or not known. */
export type SightTone = 'ok' | 'warn' | 'blocked' | 'unknown';

/** One line of sight from the pilot to a point, as drawn. */
export interface Sight {
  from: LatLon;
  to: LatLon;
  tone: SightTone;
  /** The terrain point that blocks the line, when one is known. */
  obstruction?: LatLon | null;
}

/** Colours of the line by tone: the route's status colours, grey while unknown. */
export const SIGHT_COLORS: Readonly<Record<SightTone, string>> = {
  ok: '#35D0BA',
  warn: '#F5A524',
  blocked: '#FF6B5B',
  unknown: '#A3AFBD',
};

/**
 * The line of sight from the pilot to the point under the cursor: a dashed line coloured by the
 * margin there, with the end point and the blocking ridge marked. Drawn over the results, under
 * the route.
 */
export class LineOfSightLayer extends StyleLayerSet {
  readonly id = 'results-line-of-sight';
  readonly title = 'Line of sight';
  readonly group = 'results' as const;
  override readonly order = 50;
  override readonly internal = true;
  private pending: FeatureCollection = { type: 'FeatureCollection', features: [] };

  /** Margin in dB → tone; ≥ 10 dB is comfortable, 0–10 dB works with little room, below 0 dB fails. */
  static tone(marginDb: number | null | undefined): SightTone {
    if (marginDb === null || marginDb === undefined || Number.isNaN(marginDb)) return 'unknown';
    if (marginDb < 0) return 'blocked';
    return marginDb < 10 ? 'warn' : 'ok';
  }

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-line-of-sight': { type: 'geojson', data: this.pending } };
  }

  protected layers(): LayerSpecification[] {
    const color = ['get', 'color'] as unknown as string;
    return [
      {
        id: 'sight-casing',
        type: 'line',
        source: 'rf-line-of-sight',
        filter: ['==', ['geometry-type'], 'LineString'],
        paint: { 'line-color': '#0B0F14', 'line-width': 6, 'line-opacity': 0.6 },
      },
      {
        id: 'sight-line',
        type: 'line',
        source: 'rf-line-of-sight',
        filter: ['==', ['geometry-type'], 'LineString'],
        layout: { 'line-cap': 'round' },
        paint: { 'line-color': color, 'line-width': 2.5, 'line-dasharray': [3, 2] },
      },
      {
        id: 'sight-points',
        type: 'circle',
        source: 'rf-line-of-sight',
        filter: ['==', ['geometry-type'], 'Point'],
        paint: {
          'circle-radius': 6,
          'circle-color': '#0B0F14',
          'circle-stroke-color': color,
          'circle-stroke-width': 2.5,
        },
      },
    ];
  }

  /** Draw a line of sight; null clears it. */
  setSight(sight: Sight | null): void {
    this.pending = LineOfSightLayer.features(sight);
    (this.map?.getSource('rf-line-of-sight') as GeoJSONSource | undefined)
      ?.setData(this.pending)
      .catch((e: unknown) => console.error('Line of sight could not be updated', e));
  }

  static features(sight: Sight | null): FeatureCollection {
    if (!sight) return { type: 'FeatureCollection', features: [] };
    const color = SIGHT_COLORS[sight.tone];
    const at = (p: LatLon, kind: string): Feature => ({
      type: 'Feature',
      properties: { kind, color },
      geometry: { type: 'Point', coordinates: [p.lon, p.lat] },
    });
    const features: Feature[] = [
      {
        type: 'Feature',
        properties: { kind: 'line', color },
        geometry: {
          type: 'LineString',
          coordinates: [
            [sight.from.lon, sight.from.lat],
            [sight.to.lon, sight.to.lat],
          ],
        },
      },
      at(sight.to, 'end'),
    ];
    if (sight.obstruction)
      features.push({
        ...at(sight.obstruction, 'obstruction'),
        properties: { kind: 'obstruction', color: SIGHT_COLORS.blocked },
      });
    return { type: 'FeatureCollection', features };
  }
}
