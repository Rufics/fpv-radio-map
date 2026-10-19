import type { Feature, FeatureCollection } from 'geojson';
import type { GeoJSONSource, LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import { MissingDataArea } from '../models/api.models';
import { StyleLayerSet } from '../layers/layer-definition';

export interface MissingDataShape {
  area?: MissingDataArea | null;
  /** Drone positions whose path from the pilot lacks DEM data (LegResult.missing) */
  positions?: { lat?: number; lon?: number }[];
}

/**
 * Where terrain data is missing: the [west, south, east, north] box of the DEM samples without a value,
 * outlined (dashed) with a light fill, and the drone positions that could not be evaluated.
 * Nothing inside is drawn as if it were known.
 */
export class MissingDataLayer extends StyleLayerSet {
  readonly id = 'results-missing-data';
  readonly title = 'Missing elevation data';
  readonly group = 'results' as const;
  override readonly order = 2;
  override readonly legend = 'Dashed box: no elevation data, not calculated';
  private data: FeatureCollection = { type: 'FeatureCollection', features: [] };

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-missing': { type: 'geojson', data: this.data } };
  }

  protected layers(): LayerSpecification[] {
    return [
      { id: 'missing-fill', type: 'fill', source: 'rf-missing', filter: ['==', ['get', 'kind'], 'area'], paint: { 'fill-color': '#8A96A5', 'fill-opacity': 0.18 } },
      { id: 'missing-line', type: 'line', source: 'rf-missing', filter: ['==', ['get', 'kind'], 'area'], paint: { 'line-color': '#F5A524', 'line-width': 2, 'line-dasharray': [3, 2] } },
      {
        id: 'missing-points',
        type: 'circle',
        source: 'rf-missing',
        filter: ['==', ['get', 'kind'], 'position'],
        paint: { 'circle-radius': 4, 'circle-color': '#0B0F14', 'circle-stroke-color': '#F5A524', 'circle-stroke-width': 2 },
      },
    ];
  }

  setShapes(shapes: MissingDataShape[]): void {
    const features: Feature[] = [];
    for (const s of shapes) {
      const b = s.area?.bbox;
      if (b?.length === 4) {
        const [w, so, e, n] = b;
        features.push({
          type: 'Feature',
          properties: { kind: 'area', source: s.area?.source ?? '' },
          geometry: { type: 'Polygon', coordinates: [[[w, so], [e, so], [e, n], [w, n], [w, so]]] },
        });
      }
      for (const p of s.positions ?? []) {
        if (p.lat === undefined || p.lon === undefined) continue;
        features.push({ type: 'Feature', properties: { kind: 'position' }, geometry: { type: 'Point', coordinates: [p.lon, p.lat] } });
      }
    }
    this.data = { type: 'FeatureCollection', features };
    this.pushData(this.map);
  }

  override attach(map: MlMap, beforeId: string | undefined): void {
    super.attach(map, beforeId);
    this.pushData(map);
  }

  private pushData(map: MlMap | undefined): void {
    (map?.getSource('rf-missing') as GeoJSONSource | undefined)
      ?.setData(this.data)
      .catch((e: unknown) => console.error('Missing data shapes could not be updated', e));
  }
}
