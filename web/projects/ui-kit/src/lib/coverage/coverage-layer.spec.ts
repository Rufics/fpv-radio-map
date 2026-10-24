import type { FeatureCollection } from 'geojson';
import type { LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { CoverageLayer, LimitsLayer, gridBounds } from './coverage-layer';

/** A GeoJSON source whose setData resolves (rejects when `failing` is set); also an image source. */
class FakeSource {
  data?: FeatureCollection;
  image?: { url: string };
  failing = false;
  updateImage(image: { url: string }): this {
    this.image = image;
    return this;
  }
  setData(fc: FeatureCollection): Promise<void> {
    this.data = fc;
    return this.failing ? Promise.reject(new Error('worker gone')) : Promise.resolve();
  }
}

/** The part of a MapLibre map a StyleLayerSet with a GeoJSON source uses. */
class FakeMap {
  readonly sources = new Map<string, FakeSource>();
  readonly layers: string[] = [];
  readonly visibility = new Map<string, string>();
  readonly paint = new Map<string, unknown>();
  readonly specs = new Map<string, LayerSpecification>();
  setLayoutProperty(id: string, _name: string, value: string) {
    this.visibility.set(id, value);
  }
  setPaintProperty(id: string, name: string, value: unknown) {
    this.paint.set(`${id}.${name}`, value);
  }
  getLayersOrder() {
    return [...this.layers];
  }
  removeLayer(id: string) {
    this.layers.splice(this.layers.indexOf(id), 1);
  }
  removeSource(id: string) {
    this.sources.delete(id);
  }
  getSource(id: string) {
    return this.sources.get(id);
  }
  addSource(id: string, _src: SourceSpecification) {
    this.sources.set(id, new FakeSource());
  }
  getLayer(id: string) {
    return this.layers.includes(id) ? { id } : undefined;
  }
  addLayer(layer: LayerSpecification, before?: string) {
    this.specs.set(layer.id, layer);
    const at = before ? this.layers.indexOf(before) : -1;
    if (at < 0) this.layers.push(layer.id);
    else this.layers.splice(at, 0, layer.id);
  }
  asMap(): MlMap {
    return this as unknown as MlMap;
  }
}

describe('gridBounds', () => {
  it('uses a complete bbox as is and ignores a partial one', () => {
    expect(gridBounds({ bbox: [1, 2, 3, 4] })).toEqual([1, 2, 3, 4]);
    expect(gridBounds({ bbox: [1, 2] as never })).toBeNull();
  });
});

describe('CoverageLayer', () => {
  it('clears the image and hides the heatmap for no grid or a grid without margins', () => {
    const map = new FakeMap();
    const layer = new CoverageLayer();
    layer.attach(map.asMap(), undefined);
    for (const grid of [null, { cols: 2, rows: 2 }]) {
      map.visibility.clear();
      layer.setGrid(grid);
      expect(map.sources.get('rf-coverage')!.image!.url).toMatch(/^data:image\/png;base64,/);
      expect(map.visibility.get('coverage-heat')).toBe('none');
    }
  });
});

describe('CoverageLayer relief', () => {
  const dem = { type: 'raster-dem', url: 'pmtiles:///tiles/terrain.pmtiles', encoding: 'terrarium' } as SourceSpecification;
  const grid = { marginDb: [5, 25, null, 40], cols: 2, rows: 2, bbox: [35, 48, 35.1, 48.1] as [number, number, number, number] };

  it('drapes a lighter heatmap under a relief shade in 3D, and takes the shade away on the flat map', () => {
    const map = new FakeMap();
    map.layers.push('route-line');
    const layer = new CoverageLayer();
    layer.attach(map.asMap(), 'route-line');
    layer.setGrid(grid);
    layer.setRelief(dem);
    expect(map.layers).toEqual(['coverage-heat', 'coverage-relief', 'route-line']);
    expect(map.specs.get('coverage-relief')?.type).toBe('hillshade');
    expect(map.paint.get('coverage-heat.raster-opacity')).toBe(0.38);
    expect(map.paint.get('coverage-heat.raster-saturation')).toBe(-0.35);
    expect(map.visibility.get('coverage-relief')).toBe('visible');

    layer.setVisible(map.asMap(), false);
    expect(map.visibility.get('coverage-relief')).toBe('none');

    layer.setRelief(null);
    expect(map.layers).toEqual(['coverage-heat', 'route-line']);
    expect(map.sources.has('rf-coverage-relief')).toBe(false);
    expect(map.paint.get('coverage-heat.raster-opacity')).toBe(0.55);
    expect(map.paint.get('coverage-heat.raster-saturation')).toBe(0);
  });

  it('keeps the relief across a re-attach and removes it on detach', () => {
    const map = new FakeMap();
    const layer = new CoverageLayer();
    layer.setRelief(dem);
    layer.attach(map.asMap(), undefined);
    expect(map.layers).toEqual(['coverage-heat', 'coverage-relief']);
    // no grid yet: the shade is hidden with the heatmap
    expect(map.visibility.get('coverage-relief')).toBe('none');
    layer.detach(map.asMap());
    expect(map.layers).toEqual([]);
    expect(map.sources.size).toBe(0);
  });
});

describe('LimitsLayer', () => {
  afterEach(() => vi.restoreAllMocks());

  const ring = { type: 'Polygon' as const, coordinates: [[[0, 0], [1, 0], [1, 1], [0, 0]]] };

  it('draws the radio limit as lines and the battery limit as a circle around the pilot', () => {
    const map = new FakeMap();
    const layer = new LimitsLayer();
    layer.attach(map.asMap(), undefined);
    layer.setShapes({ radio: ring, radioLabel: '0 dB', battery: { lat: 48.5, lon: 35, radiusM: 1000 } });
    const fc = map.sources.get('rf-limits')!.data!;
    expect(fc.features.map((f) => [f.properties?.['kind'], f.geometry.type])).toEqual([
      ['radio', 'MultiLineString'],
      ['battery', 'LineString'],
    ]);
    expect(fc.features[0].properties?.['label']).toBe('0 dB');
  });

  it('pushes shapes set before the map was ready when it attaches', () => {
    const layer = new LimitsLayer();
    layer.setShapes({ radio: { type: 'Feature', properties: {}, geometry: ring } });
    const map = new FakeMap();
    layer.attach(map.asMap(), undefined);
    expect(map.sources.get('rf-limits')!.data!.features).toHaveLength(1);
  });

  it('logs a failed source update', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    const map = new FakeMap();
    const layer = new LimitsLayer();
    layer.attach(map.asMap(), undefined);
    map.sources.get('rf-limits')!.failing = true;
    layer.setShapes({});
    await vi.waitFor(() => expect(error).toHaveBeenCalledWith('Limit shapes could not be updated', expect.any(Error)));
  });
});
