import { describe, expect, it } from 'vitest';
import type { LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import { StyleLayerSet } from './layer-definition';
import { LayerRegistry } from './layer-registry';
import { ContoursLayer, DarkBaseLayer, SatelliteLayer, TopoBaseLayer, createStandardLayers } from './standard-layers';

/** The part of a MapLibre map the registry uses: sources, ordered style layers, visibility. */
class FakeMap {
  readonly sources = new Map<string, SourceSpecification>();
  readonly layers: { id: string; visibility?: string }[] = [];
  getSource(id: string) {
    return this.sources.get(id);
  }
  addSource(id: string, src: SourceSpecification) {
    this.sources.set(id, src);
  }
  removeSource(id: string) {
    this.sources.delete(id);
  }
  getLayer(id: string) {
    return this.layers.find((l) => l.id === id);
  }
  addLayer(layer: LayerSpecification, beforeId?: string) {
    const i = beforeId ? this.layers.findIndex((l) => l.id === beforeId) : -1;
    if (i >= 0) this.layers.splice(i, 0, { id: layer.id });
    else this.layers.push({ id: layer.id });
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
}

/** A plain (not lazy) layer above the base maps, like the coverage heatmap. */
class ResultsLayer extends StyleLayerSet {
  readonly id = 'results-test';
  readonly title = 'Results';
  readonly group = 'results' as const;
  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-results': { type: 'geojson', data: { type: 'FeatureCollection', features: [] } } };
  }
  protected layers(): LayerSpecification[] {
    return [{ id: 'results', type: 'fill', source: 'rf-results' }];
  }
}

function setup() {
  const map = new FakeMap();
  const registry = new LayerRegistry();
  registry.register(new DarkBaseLayer({ pmtiles: '/tiles/basemap.pmtiles' }));
  registry.register(new SatelliteLayer({ pmtiles: '/tiles/satellite.pmtiles' }));
  registry.register(
    new ContoursLayer({ pmtiles: '/tiles/contours.pmtiles', sourceLayer: 'contours' }),
  );
  registry.register(new ResultsLayer());
  registry.connect(map as unknown as MlMap);
  return { map, registry };
}

describe('LayerRegistry with lazy tile layers', () => {
  it('adds the sources of shown layers only', () => {
    const { map } = setup();
    expect([...map.sources.keys()].sort()).toEqual(['rf-base', 'rf-results']);
    expect(map.getLayer('satellite')).toBeUndefined();
    expect(map.getLayer('contours-major')).toBeUndefined();
  });

  it('adds a layer when it is first shown, below the layers of higher groups', () => {
    const { map, registry } = setup();
    registry.setVisible('base-satellite', true);
    expect(map.sources.has('rf-satellite')).toBe(true);
    const ids = map.layers.map((l) => l.id);
    expect(ids.indexOf('satellite')).toBeLessThan(ids.indexOf('results'));
    expect(map.getLayer('satellite')?.visibility).toBe('visible');
    // the base maps are pick-one: the vector map is hidden, not removed
    expect(registry.isVisible('base-dark')).toBe(false);
    expect(map.getLayer('base-water')?.visibility).toBe('none');
  });

  it('keeps a shown layer on the map when it is hidden again', () => {
    const { map, registry } = setup();
    registry.setVisible('terrain-contours', true);
    registry.setVisible('terrain-contours', false);
    expect(map.sources.has('rf-contours')).toBe(true);
    expect(map.getLayer('contours-major')?.visibility).toBe('none');
  });

  it('removes only what it added when a layer is unregistered', () => {
    const { map, registry } = setup();
    registry.unregister('base-satellite');
    registry.unregister('base-dark');
    expect(map.sources.has('rf-base')).toBe(false);
    expect(map.getLayer('base-water')).toBeUndefined();
  });
});

describe('LayerRegistry suppression', () => {
  it('hides suppressed layers without touching their toggles, and shows them again', () => {
    const { map, registry } = setup();
    registry.suppress(['results-test']);
    expect(map.getLayer('results')?.visibility).toBe('none');
    expect(registry.isVisible('results-test')).toBe(true);
    // toggling while suppressed keeps it hidden
    registry.setVisible('results-test', true);
    expect(map.getLayer('results')?.visibility).toBe('none');
    registry.suppress([]);
    expect(map.getLayer('results')?.visibility).toBe('visible');
  });

  it('keeps a lazy layer hidden when it is first shown while suppressed', () => {
    const { map, registry } = setup();
    registry.suppress(['terrain-contours']);
    registry.setVisible('terrain-contours', true);
    expect(map.getLayer('contours-major')?.visibility).toBe('none');
    registry.suppress([]);
    expect(map.getLayer('contours-major')?.visibility).toBe('visible');
  });
});

describe('TopoBaseLayer', () => {
  it('is a third pick-one base map, made only when the server has elevation tiles', () => {
    const ids = (dem: boolean) =>
      createStandardLayers(
        {
          basemap: { pmtiles: '/tiles/basemap.pmtiles' },
          satellite: { pmtiles: '/tiles/satellite.pmtiles' },
          ...(dem ? { dem: { pmtiles: '/tiles/terrain.pmtiles' } } : {}),
        },
        '2026-01-01T00:00:00Z',
      ).map((l) => l.id);
    expect(ids(true)).toEqual(expect.arrayContaining(['base-dark', 'base-satellite', 'base-topo']));
    expect(ids(false)).not.toContain('base-topo');
  });

  it('tints and shades the elevation, with contours when there are any, and replaces the other base maps', () => {
    const map = new FakeMap();
    const registry = new LayerRegistry();
    registry.register(new DarkBaseLayer({ pmtiles: '/tiles/basemap.pmtiles' }));
    registry.register(
      new TopoBaseLayer({ pmtiles: '/tiles/terrain.pmtiles' }, { pmtiles: '/tiles/contours.pmtiles', sourceLayer: 'contours' }),
    );
    registry.connect(map as unknown as MlMap);
    expect(map.getLayer('topo-tint')).toBeUndefined();
    registry.setVisible('base-topo', true);
    expect(map.sources.get('rf-topo-dem')).toMatchObject({ type: 'raster-dem', url: 'pmtiles:///tiles/terrain.pmtiles' });
    expect(map.layers.filter((l) => l.id.startsWith('topo-')).map((l) => l.id)).toEqual([
      'topo-bg',
      'topo-tint',
      'topo-shade',
      'topo-contours-minor',
      'topo-contours-index',
    ]);
    expect(registry.isVisible('base-dark')).toBe(false);
    expect(map.getLayer('base-water')?.visibility).toBe('none');
  });

  it('draws without contours when the server has none', () => {
    const map = new FakeMap();
    const registry = new LayerRegistry();
    registry.register(new TopoBaseLayer({ tiles: ['mockdem://{z}/{x}/{y}'] }));
    registry.connect(map as unknown as MlMap);
    registry.setVisible('base-topo', true);
    expect(map.sources.has('rf-topo-contours')).toBe(false);
    expect(map.getLayer('topo-contours-minor')).toBeUndefined();
    expect(map.getLayer('topo-shade')).toBeDefined();
  });
});
