import type { FeatureCollection } from 'geojson';
import type { LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { MissingDataLayer } from './missing-data-layer';

/** A GeoJSON source whose setData resolves, or rejects when `failing` is set. */
class FakeGeoJsonSource {
  data?: FeatureCollection;
  failing = false;
  setData(fc: FeatureCollection): Promise<void> {
    this.data = fc;
    return this.failing ? Promise.reject(new Error('worker gone')) : Promise.resolve();
  }
}

/** The part of a MapLibre map a StyleLayerSet with a GeoJSON source uses. */
class FakeMap {
  readonly sources = new Map<string, FakeGeoJsonSource>();
  readonly layers: string[] = [];
  getSource(id: string) {
    return this.sources.get(id);
  }
  addSource(id: string, _src: SourceSpecification) {
    this.sources.set(id, new FakeGeoJsonSource());
  }
  getLayer(id: string) {
    return this.layers.includes(id) ? { id } : undefined;
  }
  addLayer(layer: LayerSpecification) {
    this.layers.push(layer.id);
  }
  asMap(): MlMap {
    return this as unknown as MlMap;
  }
}

describe('MissingDataLayer', () => {
  afterEach(() => vi.restoreAllMocks());

  it('draws the missing DEM box as a closed polygon and the positions as points', () => {
    const map = new FakeMap();
    const layer = new MissingDataLayer();
    layer.attach(map.asMap(), undefined);
    layer.setShapes([
      { area: { bbox: [35, 48, 35.2, 48.1], source: 'copernicus' }, positions: [{ lat: 48.05, lon: 35.1 }, { lat: 48 }] },
      { area: { bbox: [1, 2] as never } },
    ]);
    const fc = map.sources.get('rf-missing')!.data!;
    expect(fc.features.map((f) => f.properties?.['kind'])).toEqual(['area', 'position']);
    expect(fc.features[0].properties?.['source']).toBe('copernicus');
    expect(fc.features[0].geometry).toEqual({
      type: 'Polygon',
      coordinates: [[[35, 48], [35.2, 48], [35.2, 48.1], [35, 48.1], [35, 48]]],
    });
    expect(fc.features[1].geometry).toEqual({ type: 'Point', coordinates: [35.1, 48.05] });
  });

  it('keeps shapes set before the map is ready and pushes them on attach', () => {
    const layer = new MissingDataLayer();
    layer.setShapes([{ positions: [{ lat: 1, lon: 2 }] }]);
    const map = new FakeMap();
    layer.attach(map.asMap(), undefined);
    expect(map.sources.get('rf-missing')!.data!.features).toHaveLength(1);
    expect(map.layers).toEqual(['missing-fill', 'missing-line', 'missing-points']);
  });

  it('logs a failed source update instead of leaving the promise unhandled', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    const map = new FakeMap();
    const layer = new MissingDataLayer();
    layer.attach(map.asMap(), undefined);
    map.sources.get('rf-missing')!.failing = true;
    layer.setShapes([]);
    await vi.waitFor(() => expect(error).toHaveBeenCalledWith('Missing data shapes could not be updated', expect.any(Error)));
  });
});
