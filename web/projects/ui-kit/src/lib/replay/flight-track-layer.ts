import type { FeatureCollection, LineString, Point } from 'geojson';
import type { GeoJSONSource, LayerSpecification, SourceSpecification } from 'maplibre-gl';
import { StyleLayerSet } from '../layers/layer-definition';
import { LatLon } from '../tools/geo';
import { TrackSegment } from './flight-replay';

const EMPTY_LINES: FeatureCollection<LineString> = { type: 'FeatureCollection', features: [] };
const EMPTY_POINTS: FeatureCollection<Point> = { type: 'FeatureCollection', features: [] };

/**
 * The flown track of a replayed flight, coloured by the chosen value, and the craft at the
 * replay time. Drawn over the planned route, which stays visible under it for plan against fact.
 */
export class FlightTrackLayer extends StyleLayerSet {
  readonly id = 'mission-flight-track';
  readonly title = 'Flown track';
  readonly group = 'mission' as const;
  override readonly order = 2;
  override readonly internal = true;
  private track: FeatureCollection<LineString> = EMPTY_LINES;
  private cursor: FeatureCollection<Point> = EMPTY_POINTS;

  protected sources(): Record<string, SourceSpecification> {
    return {
      'rf-flight-track': { type: 'geojson', data: this.track },
      'rf-flight-cursor': { type: 'geojson', data: this.cursor },
    };
  }

  protected layers(): LayerSpecification[] {
    const layout = { 'line-cap': 'round', 'line-join': 'round' } as const;
    return [
      { id: 'flight-track-casing', type: 'line', source: 'rf-flight-track', layout, paint: { 'line-color': '#0B0F14', 'line-width': 7, 'line-opacity': 0.9 } },
      { id: 'flight-track', type: 'line', source: 'rf-flight-track', layout, paint: { 'line-color': ['get', 'color'], 'line-width': 3.5 } },
      { id: 'flight-cursor-halo', type: 'circle', source: 'rf-flight-cursor', paint: { 'circle-radius': 11, 'circle-color': '#E7ECF2', 'circle-opacity': 0.25 } },
      {
        id: 'flight-cursor',
        type: 'circle',
        source: 'rf-flight-cursor',
        paint: { 'circle-radius': 6, 'circle-color': '#E7ECF2', 'circle-stroke-color': '#0B0F14', 'circle-stroke-width': 2 },
      },
    ];
  }

  setTrack(segments: readonly TrackSegment[]): void {
    this.track = {
      type: 'FeatureCollection',
      features: segments.map((s) => ({
        type: 'Feature',
        properties: { color: s.color },
        geometry: { type: 'LineString', coordinates: s.points.map((p) => [p.lon, p.lat]) },
      })),
    };
    this.update('rf-flight-track', this.track);
  }

  setCursor(at: LatLon | null): void {
    this.cursor = at
      ? { type: 'FeatureCollection', features: [{ type: 'Feature', properties: {}, geometry: { type: 'Point', coordinates: [at.lon, at.lat] } }] }
      : EMPTY_POINTS;
    this.update('rf-flight-cursor', this.cursor);
  }

  private update(source: string, data: FeatureCollection): void {
    (this.map?.getSource(source) as GeoJSONSource | undefined)?.setData(data).catch((e: unknown) => console.error('Flight track could not be updated', e));
  }
}
