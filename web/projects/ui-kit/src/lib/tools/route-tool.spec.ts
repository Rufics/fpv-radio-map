import type { FeatureCollection } from 'geojson';
import type { LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { LayerRegistry } from '../layers/layer-registry';
import { MapContext } from '../map/map-context';
import { RouteDrawing, RouteTool } from './route-tool';

/** A GeoJSON source whose setData resolves, or rejects when `failing` is set. */
class FakeGeoJsonSource {
  data?: FeatureCollection;
  failing = false;
  setData(fc: FeatureCollection): Promise<void> {
    this.data = fc;
    return this.failing ? Promise.reject(new Error('worker gone')) : Promise.resolve();
  }
}

/** The part of a MapLibre map the layer registry and the route lines use. */
class FakeMap {
  readonly sources = new Map<string, FakeGeoJsonSource>();
  readonly layers: { id: string; visibility?: string }[] = [];
  getSource(id: string) {
    return this.sources.get(id);
  }
  addSource(id: string, _src: SourceSpecification) {
    this.sources.set(id, new FakeGeoJsonSource());
  }
  removeSource(id: string) {
    this.sources.delete(id);
  }
  getLayer(id: string) {
    return this.layers.find((l) => l.id === id);
  }
  addLayer(layer: LayerSpecification) {
    this.layers.push({ id: layer.id });
  }
  removeLayer(id: string) {
    this.layers.splice(
      this.layers.findIndex((l) => l.id === id),
      1,
    );
  }
  setLayoutProperty(id: string, _name: string, value: string) {
    const l = this.getLayer(id);
    if (l) l.visibility = value;
  }
  asMap(): MlMap {
    return this as unknown as MlMap;
  }
}

const twoLegs: RouteDrawing = {
  pilot: { lat: 48.5, lon: 35.0 },
  waypoints: [
    { lat: 48.51, lon: 35.01 },
    { lat: 48.52, lon: 35.02 },
  ],
  legs: [{ status: 'ok' }, { status: 'blocked' }],
  returnLeg: { from: { lat: 48.52, lon: 35.02 } },
};

describe('RouteTool lines', () => {
  afterEach(() => vi.restoreAllMocks());

  function setup() {
    const registry = new LayerRegistry();
    const ctx = new MapContext();
    const tool = new RouteTool(registry, ctx);
    return { registry, ctx, tool };
  }

  it('registers the route lines as an internal mission layer and removes them on destroy', () => {
    const { registry, tool } = setup();
    const def = registry.get('mission-route');
    expect(def?.internal).toBe(true);
    expect(def?.group).toBe('mission');
    tool.destroy();
    expect(registry.get('mission-route')).toBeUndefined();
  });

  it('pushes legs drawn before the map was ready once the layer attaches', () => {
    const { registry, tool } = setup();
    tool.draw(twoLegs);
    const map = new FakeMap();
    registry.connect(map.asMap());
    const fc = map.sources.get('rf-route')!.data!;
    expect(fc.features.map((f) => f.properties?.['status'])).toEqual(['ok', 'blocked', 'return']);
    expect(fc.features[1].geometry).toEqual({ type: 'LineString', coordinates: [[35.01, 48.51], [35.02, 48.52]] });
    expect(fc.features[2].geometry).toEqual({ type: 'LineString', coordinates: [[35.02, 48.52], [35.0, 48.5]] });
  });

  it('replaces the lines on the live map when the route is cleared', () => {
    const { registry, ctx, tool } = setup();
    tool.draw(twoLegs);
    const map = new FakeMap();
    registry.connect(map.asMap());
    ctx.map.set(map.asMap());
    tool.draw({ pilot: null, waypoints: [], legs: [{ status: 'ok' }] });
    expect(map.sources.get('rf-route')!.data!.features).toEqual([]);
  });

  it('logs a failed source update', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    const { registry, tool } = setup();
    tool.draw(twoLegs);
    const map = new FakeMap();
    map.addSource('rf-route', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } });
    map.sources.get('rf-route')!.failing = true;
    registry.connect(map.asMap());
    await vi.waitFor(() => expect(error).toHaveBeenCalledWith('Route lines could not be updated', expect.any(Error)));
  });

  it('forwards map clicks to the add-point handler only while one is set', () => {
    const { ctx, tool } = setup();
    const added: { lat: number; lon: number }[] = [];
    tool.setAddPointHandler((p) => added.push(p));
    ctx.emitClick({ lat: 1, lon: 2 });
    tool.setAddPointHandler(null);
    ctx.emitClick({ lat: 3, lon: 4 });
    expect(added).toEqual([{ lat: 1, lon: 2 }]);
  });
});
