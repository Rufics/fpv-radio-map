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
  setLayoutProperty(id: string, _name: string, value: string) {
    this.visibility.set(id, value);
  }
  readonly paint = new Map<string, unknown>();
  setPaintProperty(id: string, name: string, value: unknown) {
    this.paint.set(`${id}.${name}`, value);
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
  addLayer(layer: LayerSpecification) {
    this.layers.push(layer.id);
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

describe('CoverageLayer opacity and place', () => {
  it('is drawn over the base maps and under the terrain group', () => {
    const layer = new CoverageLayer();
    expect(layer.group).toBe('results');
    expect(layer.drawGroup).toBe('base');
    expect(layer.order).toBeGreaterThan(9);
  });

  it('keeps an opacity set before the map and applies it on attach, clamped to 0..1', () => {
    const layer = new CoverageLayer();
    layer.setOpacity(0.12);
    const map = new FakeMap();
    layer.attach(map.asMap(), undefined);
    expect(map.paint.get('coverage-heat.raster-opacity')).toBe(0.12);
    layer.setOpacity(4);
    expect(map.paint.get('coverage-heat.raster-opacity')).toBe(1);
    layer.setOpacity(-1);
    expect(map.paint.get('coverage-heat.raster-opacity')).toBe(0);
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
