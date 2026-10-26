import type { LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';

export type LayerGroup = 'base' | 'terrain' | 'weather' | 'results' | 'mission';

/** Draw order of groups, bottom to top. */
export const LAYER_GROUP_RANK: Record<LayerGroup, number> = {
  base: 0,
  terrain: 10,
  weather: 20,
  results: 30,
  mission: 40,
};

export const LAYER_GROUP_TITLE: Record<LayerGroup, string> = {
  base: 'Base · pick one',
  terrain: 'Terrain',
  weather: 'Weather · forecast time',
  results: 'Results',
  mission: 'Mission',
};

/** A toggleable map layer known to the layer registry. */
export interface LayerDefinition {
  readonly id: string;
  readonly title: string;
  readonly group: LayerGroup;
  /**
   * The group whose place in the stack the layer takes, when it is drawn somewhere else than the
   * group it is listed under (the coverage heatmap is listed with the results but drawn on the
   * ground, under the terrain shading). Default: `group`.
   */
  readonly drawGroup?: LayerGroup;
  /** Order inside the group it is drawn in, lower is drawn first. */
  readonly order?: number;
  /** Only one visible layer per exclusive group (base maps). */
  readonly exclusive?: boolean;
  readonly defaultVisible?: boolean;
  /** Hidden from the layer panel (route lines, markers). */
  readonly internal?: boolean;
  /** Short legend text shown in the layer panel. */
  readonly legend?: string;
  /** Data credit added to the attribution line while visible. */
  readonly attribution?: string;
  /**
   * Add the sources only when the layer is first shown, so a hidden layer downloads nothing, not
   * even its PMTiles header (tile layers). Once added, it stays added while hidden.
   */
  readonly lazy?: boolean;
  /** Add sources and style layers. Called once the map style is loaded, or when first shown if lazy. */
  attach(map: MlMap, beforeId: string | undefined): void;
  detach(map: MlMap): void;
  setVisible(map: MlMap, visible: boolean): void;
  /** Style layer ids owned by this definition, bottom first. */
  styleLayerIds(): string[];
}

/**
 * Base class for layers that are a fixed set of sources and style layers.
 * Subclasses provide `sources()` and `layers()`.
 */
export abstract class StyleLayerSet implements LayerDefinition {
  abstract readonly id: string;
  abstract readonly title: string;
  abstract readonly group: LayerGroup;
  readonly drawGroup?: LayerGroup;
  readonly order: number = 0;
  readonly exclusive: boolean = false;
  readonly defaultVisible: boolean = true;
  readonly internal: boolean = false;
  readonly lazy: boolean = false;
  readonly legend?: string;
  readonly attribution?: string;
  protected map?: MlMap;
  private cachedLayers?: LayerSpecification[];

  protected abstract sources(): Record<string, SourceSpecification>;
  protected abstract layers(): LayerSpecification[];

  styleLayerIds(): string[] {
    return this.layerSpecs().map((l) => l.id);
  }

  protected layerSpecs(): LayerSpecification[] {
    this.cachedLayers ??= this.layers();
    return this.cachedLayers;
  }

  attach(map: MlMap, beforeId: string | undefined): void {
    this.map = map;
    for (const [id, src] of Object.entries(this.sources())) {
      if (!map.getSource(id)) map.addSource(id, src);
    }
    for (const layer of this.layerSpecs()) {
      if (!map.getLayer(layer.id)) map.addLayer(layer, beforeId && map.getLayer(beforeId) ? beforeId : undefined);
    }
  }

  detach(map: MlMap): void {
    for (const layer of [...this.layerSpecs()].reverse()) {
      if (map.getLayer(layer.id)) map.removeLayer(layer.id);
    }
    for (const id of Object.keys(this.sources())) {
      if (map.getSource(id)) map.removeSource(id);
    }
    this.map = undefined;
  }

  setVisible(map: MlMap, visible: boolean): void {
    for (const id of this.styleLayerIds()) {
      if (map.getLayer(id)) map.setLayoutProperty(id, 'visibility', visible ? 'visible' : 'none');
    }
  }
}
