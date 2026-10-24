import type {
  ExpressionSpecification,
  FilterSpecification,
  LayerSpecification,
  RasterTileSource,
  SourceSpecification,
} from 'maplibre-gl';
import { MapSourcesConfig, TileSourceRef } from '../models/api.models';
import { LayerDefinition, StyleLayerSet } from './layer-definition';

/** Map colours (Tokens.dc.html / MapScene.dc.html). */
export const MAP_COLORS = {
  land: '#0E1319',
  landAlt: '#121A22',
  water: '#12304A',
  waterLine: '#1E4B66',
  road: '#333B46',
  roadMajor: '#3F4955',
  boundary: '#3A4757',
  contour: '#243140',
  contourMajor: '#2F3E50',
};

function vectorSource(ref: TileSourceRef): SourceSpecification {
  if (ref.pmtiles) return { type: 'vector', url: `pmtiles://${ref.pmtiles}`, attribution: ref.attribution };
  if (ref.tiles) return { type: 'vector', tiles: ref.tiles, maxzoom: ref.maxzoom ?? 14, attribution: ref.attribution };
  return { type: 'geojson', data: ref.geojson ?? { type: 'FeatureCollection', features: [] } };
}

/** A raster tile source (pre-rendered hillshade, satellite) from a PMTiles archive or XYZ templates. */
export function rasterSource(ref: TileSourceRef): SourceSpecification {
  if (ref.pmtiles) return { type: 'raster', url: `pmtiles://${ref.pmtiles}`, tileSize: ref.tileSize ?? 256 };
  return { type: 'raster', tiles: ref.tiles ?? [], tileSize: ref.tileSize ?? 256, maxzoom: ref.maxzoom ?? 16 };
}

/** A raster-dem source (Terrarium or Mapbox encoding) from a PMTiles archive or XYZ templates. */
export function demSource(ref: TileSourceRef): SourceSpecification {
  const encoding = ref.encoding ?? 'terrarium';
  if (ref.pmtiles) return { type: 'raster-dem', url: `pmtiles://${ref.pmtiles}`, encoding, tileSize: ref.tileSize ?? 256 };
  return { type: 'raster-dem', tiles: ref.tiles ?? [], encoding, tileSize: ref.tileSize ?? 256, maxzoom: ref.maxzoom ?? 12 };
}

/**
 * Dark vector base map. With a PMTiles archive it reads the OpenMapTiles schema produced by
 * tools/tiles-builder (source layers water, waterway, landcover, landuse, transportation, boundary);
 * with GeoJSON (dev mock) each feature carries a `layer` property with the same names.
 */
export class DarkBaseLayer extends StyleLayerSet {
  readonly id = 'base-dark';
  readonly title = 'Map';
  readonly group = 'base' as const;
  override readonly lazy = true;
  override readonly exclusive = true;
  override readonly order = 0;
  override readonly attribution: string;
  private readonly vector: boolean;

  constructor(private readonly ref: TileSourceRef) {
    super();
    this.vector = !ref.geojson;
    this.attribution = ref.attribution ?? '© OpenMapTiles © OpenStreetMap contributors';
  }

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-base': vectorSource(this.ref) };
  }

  private part(layer: string, extra?: FilterSpecification): Partial<LayerSpecification> {
    if (this.vector) return { 'source-layer': layer, ...(extra ? { filter: extra } : {}) } as Partial<LayerSpecification>;
    const byLayer: ExpressionSpecification = ['==', ['get', 'layer'], layer];
    return { filter: extra ? ['all', byLayer, extra as ExpressionSpecification] : byLayer } as Partial<LayerSpecification>;
  }

  protected layers(): LayerSpecification[] {
    const major: FilterSpecification = ['in', ['get', 'class'], ['literal', ['motorway', 'trunk', 'primary']]];
    const minor: FilterSpecification = ['in', ['get', 'class'], ['literal', ['secondary', 'tertiary', 'minor', 'service', 'track']]];
    return [
      { id: 'base-bg', type: 'background', paint: { 'background-color': MAP_COLORS.land } },
      { id: 'base-landcover', type: 'fill', source: 'rf-base', ...this.part('landcover'), paint: { 'fill-color': MAP_COLORS.landAlt } } as LayerSpecification,
      { id: 'base-landuse', type: 'fill', source: 'rf-base', ...this.part('landuse'), paint: { 'fill-color': MAP_COLORS.landAlt, 'fill-opacity': 0.8 } } as LayerSpecification,
      { id: 'base-water', type: 'fill', source: 'rf-base', ...this.part('water'), paint: { 'fill-color': MAP_COLORS.water } } as LayerSpecification,
      {
        id: 'base-waterway',
        type: 'line',
        source: 'rf-base',
        ...this.part('waterway'),
        paint: { 'line-color': MAP_COLORS.waterLine, 'line-width': ['interpolate', ['linear'], ['zoom'], 8, 1.5, 14, 6] },
      } as LayerSpecification,
      { id: 'base-roads', type: 'line', source: 'rf-base', ...this.part('transportation', minor), paint: { 'line-color': MAP_COLORS.road, 'line-width': ['interpolate', ['linear'], ['zoom'], 8, 0.6, 14, 2.5] } } as LayerSpecification,
      { id: 'base-roads-major', type: 'line', source: 'rf-base', ...this.part('transportation', major), paint: { 'line-color': MAP_COLORS.roadMajor, 'line-width': ['interpolate', ['linear'], ['zoom'], 8, 1.2, 14, 4] } } as LayerSpecification,
      { id: 'base-boundaries', type: 'line', source: 'rf-base', ...this.part('boundary'), paint: { 'line-color': MAP_COLORS.boundary, 'line-dasharray': [3, 2] } } as LayerSpecification,
    ];
  }
}

export class SatelliteLayer extends StyleLayerSet {
  readonly id = 'base-satellite';
  readonly title = 'Satellite';
  readonly group = 'base' as const;
  override readonly lazy = true;
  override readonly exclusive = true;
  override readonly order = 1;
  override readonly defaultVisible = false;
  /** Only an explicit credit: the operator's own (--sat-attribution) arrives with the manifest's. */
  override readonly attribution?: string;
  constructor(private readonly ref: TileSourceRef) {
    super();
    this.attribution = ref.attribution;
  }
  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-satellite': rasterSource(this.ref) };
  }
  protected layers(): LayerSpecification[] {
    return [{ id: 'satellite', type: 'raster', source: 'rf-satellite', paint: { 'raster-saturation': -0.25, 'raster-brightness-max': 0.8 } }];
  }
}

/**
 * Hillshade. tools/tiles-builder ships it pre-rendered (raster PNG with alpha, for a dark UI);
 * a raster-dem source (`dem`, terrarium/mapbox encoding) is shaded by MapLibre instead.
 */
export class HillshadeLayer extends StyleLayerSet {
  readonly id = 'terrain-hillshade';
  readonly title = 'Hillshade';
  readonly group = 'terrain' as const;
  override readonly lazy = true;
  override readonly attribution = 'Copernicus DEM';
  constructor(
    private readonly ref: TileSourceRef,
    private readonly mode: 'raster' | 'dem',
  ) {
    super();
  }
  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-hillshade': this.mode === 'dem' ? demSource(this.ref) : rasterSource(this.ref) };
  }
  protected layers(): LayerSpecification[] {
    if (this.mode === 'raster') {
      return [{ id: 'hillshade', type: 'raster', source: 'rf-hillshade', paint: { 'raster-opacity': 0.9 } }];
    }
    return [
      {
        id: 'hillshade',
        type: 'hillshade',
        source: 'rf-hillshade',
        paint: {
          'hillshade-shadow-color': '#06090D',
          'hillshade-highlight-color': '#2A3644',
          'hillshade-accent-color': '#0B0F14',
          'hillshade-exaggeration': 0.55,
        },
      },
    ];
  }
}

/**
 * Contour lines. PMTiles from tools/tiles-builder: source layer `contours` (z12+) and
 * `contours_index` (z10+, every 100 m), attribute `ele` in metres. GeoJSON (mock): one set, index = ele % 100 == 0.
 */
export class ContoursLayer extends StyleLayerSet {
  readonly id = 'terrain-contours';
  readonly title = 'Contours';
  readonly group = 'terrain' as const;
  override readonly lazy = true;
  override readonly order = 1;
  override readonly defaultVisible = false;
  override readonly attribution = 'Copernicus DEM';
  constructor(private readonly ref: TileSourceRef) {
    super();
  }
  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-contours': vectorSource(this.ref) };
  }
  protected layers(): LayerSpecification[] {
    const index: ExpressionSpecification = ['==', ['%', ['to-number', ['get', 'ele']], 100], 0];
    if (this.ref.geojson) {
      return [
        { id: 'contours-minor', type: 'line', source: 'rf-contours', filter: ['!', index], paint: { 'line-color': MAP_COLORS.contour, 'line-width': 0.8 } },
        { id: 'contours-major', type: 'line', source: 'rf-contours', filter: index, paint: { 'line-color': MAP_COLORS.contourMajor, 'line-width': 1.4 } },
      ];
    }
    const minorLayer = this.ref.sourceLayer ?? 'contours';
    return [
      { id: 'contours-minor', type: 'line', source: 'rf-contours', 'source-layer': minorLayer, minzoom: 12, filter: ['!', index], paint: { 'line-color': MAP_COLORS.contour, 'line-width': 0.8 } },
      { id: 'contours-major', type: 'line', source: 'rf-contours', 'source-layer': `${minorLayer}_index`, minzoom: 10, paint: { 'line-color': MAP_COLORS.contourMajor, 'line-width': 1.4 } },
    ];
  }
}

/**
 * Topographic base map from the elevation raster-dem: hypsometric tints (stops dense at low
 * heights, so plains still show their rises), hill shading over them and, when the server has
 * them, contour lines every 20 m with darker index lines every 100 m.
 */
export class TopoBaseLayer extends StyleLayerSet {
  readonly id = 'base-topo';
  readonly title = 'Topo';
  readonly group = 'base' as const;
  override readonly lazy = true;
  override readonly exclusive = true;
  override readonly order = 2;
  override readonly defaultVisible = false;
  override readonly attribution = 'Copernicus DEM';
  constructor(
    private readonly dem: TileSourceRef,
    private readonly contours?: TileSourceRef,
  ) {
    super();
  }
  protected sources(): Record<string, SourceSpecification> {
    const out: Record<string, SourceSpecification> = { 'rf-topo-dem': demSource(this.dem) };
    if (this.contours) out['rf-topo-contours'] = vectorSource(this.contours);
    return out;
  }
  protected layers(): LayerSpecification[] {
    const stops = TOPO_TINTS.flatMap(([m, c]) => [m, c]);
    const layers: LayerSpecification[] = [
      { id: 'topo-bg', type: 'background', paint: { 'background-color': MAP_COLORS.land } },
      {
        id: 'topo-tint',
        type: 'color-relief',
        source: 'rf-topo-dem',
        paint: { 'color-relief-color': ['interpolate', ['linear'], ['elevation'], ...stops], 'color-relief-opacity': 0.9 },
      } as unknown as LayerSpecification,
      {
        id: 'topo-shade',
        type: 'hillshade',
        source: 'rf-topo-dem',
        paint: {
          'hillshade-shadow-color': 'rgba(4, 7, 11, 0.7)',
          'hillshade-highlight-color': 'rgba(255, 255, 255, 0.22)',
          'hillshade-accent-color': 'rgba(4, 7, 11, 0.3)',
          'hillshade-exaggeration': 0.55,
        },
      },
    ];
    if (!this.contours) return layers;
    const vector = !this.contours.geojson;
    const index: ExpressionSpecification = ['==', ['%', ['to-number', ['get', 'ele']], 100], 0];
    const minorLayer = this.contours.sourceLayer ?? 'contours';
    const line = (id: string, sourceLayer: string, filter: ExpressionSpecification, color: string, opacity: number, width: number, minzoom: number) =>
      ({
        id,
        type: 'line',
        source: 'rf-topo-contours',
        ...(vector ? { 'source-layer': sourceLayer, minzoom } : {}),
        filter,
        paint: { 'line-color': color, 'line-opacity': opacity, 'line-width': width },
      }) as LayerSpecification;
    return [
      ...layers,
      line('topo-contours-minor', minorLayer, ['!', index], '#0B0F14', 0.3, 0.7, 12),
      line('topo-contours-index', vector ? `${minorLayer}_index` : minorLayer, index, '#0B0F14', 0.5, 1.2, 10),
    ];
  }
}

/** Hypsometric tints for a dark UI, metres above sea level → colour. */
export const TOPO_TINTS: [number, string][] = [
  [0, '#1D3B33'],
  [100, '#2B4A35'],
  [200, '#3D5A37'],
  [300, '#56673B'],
  [500, '#6F7142'],
  [800, '#86784C'],
  [1200, '#8E7158'],
  [2000, '#9A8A7C'],
  [3000, '#C9C4BC'],
];

export type WeatherKind = 'wind' | 'precipitation' | 'temperature';

const WEATHER_TITLES: Record<WeatherKind, string> = {
  wind: 'Wind at flight height',
  precipitation: 'Precipitation',
  temperature: 'Temperature',
};

/** Forecast raster tiles; the time is part of the tile URL and changes with the slider. */
export class WeatherLayer extends StyleLayerSet {
  readonly id: string;
  readonly title: string;
  readonly group = 'weather' as const;
  override readonly lazy = true;
  override readonly order: number;
  override readonly defaultVisible: boolean;
  override readonly attribution = 'Weather: Open-Meteo (ICON, GFS, ECMWF open data)';
  private timeIso: string;

  constructor(
    private readonly ref: TileSourceRef,
    readonly kind: WeatherKind,
    timeIso: string,
  ) {
    super();
    this.id = `weather-${kind}`;
    this.title = WEATHER_TITLES[kind];
    this.order = ['wind', 'precipitation', 'temperature'].indexOf(kind);
    this.defaultVisible = false;
    this.timeIso = timeIso;
  }

  private templates(): string[] {
    const hour = this.timeIso.slice(0, 13) + ':00Z';
    return (this.ref.tiles ?? []).map((t) => t.replace('{layer}', this.kind).replace('{time}', encodeURIComponent(hour)));
  }

  setTime(timeIso: string): void {
    this.timeIso = timeIso;
    const src = this.map?.getSource(`rf-weather-${this.kind}`) as RasterTileSource | undefined;
    src?.setTiles(this.templates());
  }

  protected sources(): Record<string, SourceSpecification> {
    return { [`rf-weather-${this.kind}`]: { type: 'raster', tiles: this.templates(), tileSize: this.ref.tileSize ?? 256, maxzoom: this.ref.maxzoom ?? 10 } };
  }

  protected layers(): LayerSpecification[] {
    return [{ id: `weather-${this.kind}-raster`, type: 'raster', source: `rf-weather-${this.kind}`, paint: { 'raster-opacity': 0.75 } }];
  }
}

/** The standard layer set built from the server's map configuration. */
export function createStandardLayers(config: MapSourcesConfig, forecastTimeIso: string): LayerDefinition[] {
  const out: LayerDefinition[] = [];
  if (config.basemap) out.push(new DarkBaseLayer(config.basemap));
  if (config.satellite) out.push(new SatelliteLayer(config.satellite));
  if (config.dem) out.push(new TopoBaseLayer(config.dem, config.contours));
  if (config.hillshade) out.push(new HillshadeLayer(config.hillshade, 'raster'));
  else if (config.dem) out.push(new HillshadeLayer(config.dem, 'dem'));
  if (config.contours) out.push(new ContoursLayer(config.contours));
  if (config.weather) {
    for (const kind of ['wind', 'precipitation', 'temperature'] as WeatherKind[]) {
      out.push(new WeatherLayer(config.weather, kind, forecastTimeIso));
    }
  }
  return out;
}
