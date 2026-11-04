import { addProtocol } from 'maplibre-gl';
import { PMTiles, RangeResponse, Source } from 'pmtiles';
import { MapSourcesConfig, TileSourceRef } from '../models/api.models';
import { ensurePmtilesProtocol } from '../map/map-view.component';
import { SavedPackage } from './offline-storage';
import { ZipIndex } from './zip-index';

/** One PMTiles archive inside a saved package, read by byte ranges of the zip. */
class ZipEntrySource implements Source {
  constructor(
    private readonly key: string,
    private readonly zip: ZipIndex,
    private readonly name: string,
  ) {}

  getKey(): string {
    return this.key;
  }

  async getBytes(offset: number, length: number): Promise<RangeResponse> {
    return { data: await this.zip.bytes(this.name, offset, length) };
  }
}

type LayerKey = 'basemap' | 'satellite' | 'hillshade' | 'dem' | 'contours';

/**
 * The map read from a package saved on this device: its tiles through the pmtiles:// protocol,
 * its fonts through rfoffline://. A package lists its layers by the tiles-builder names; the map
 * configuration calls the terrain heights `dem`.
 */
export class OfflineMapSources {
  static readonly protocol = 'rfoffline';
  private static readonly layerKeys: Record<string, LayerKey> = {
    basemap: 'basemap',
    satellite: 'satellite',
    hillshade: 'hillshade',
    terrain: 'dem',
    contours: 'contours',
  };
  private static readonly defaults: Partial<Record<LayerKey, TileSourceRef>> = {
    contours: { sourceLayer: 'contours' },
    dem: { encoding: 'terrarium' },
  };
  private static readonly opened = new Map<string, ZipIndex>();
  private static registered = false;

  /**
   * Makes the package `saved` (its zip `zip`) readable by the map, and returns the configuration
   * that reads it: the layers the package has replace those of `base` (the server's, or null
   * without a server), which keeps the rest, such as the weather.
   */
  static async open(saved: SavedPackage, zip: Blob, base: MapSourcesConfig | null): Promise<MapSourcesConfig> {
    const index = await ZipIndex.read(zip);
    OfflineMapSources.opened.set(saved.key, index);
    OfflineMapSources.registerFonts();
    const protocol = ensurePmtilesProtocol();
    const config: MapSourcesConfig = { ...base };
    for (const layer of saved.region.layers) {
      const key = OfflineMapSources.layerKeys[layer];
      const entry = `tiles/${layer}.pmtiles`;
      if (!key || !index.has(entry)) continue;
      const archive = `${OfflineMapSources.protocol}/${saved.key}/${saved.region.version}/${layer}`;
      protocol.add(new PMTiles(new ZipEntrySource(archive, index, entry)));
      config[key] = { ...OfflineMapSources.defaults[key], ...config[key], pmtiles: archive, tiles: undefined, geojson: undefined };
    }
    if (index.names().some((n) => n.startsWith('glyphs/'))) {
      config.glyphs = `${OfflineMapSources.protocol}://${saved.key}/glyphs/{fontstack}/{range}.pbf`;
    }
    const [w, s, e, n] = saved.region.bbox;
    config.center = [(w + e) / 2, (s + n) / 2];
    return config;
  }

  /** Forgets the package `key`: its zip is no longer read. */
  static close(key: string): void {
    OfflineMapSources.opened.delete(key);
  }

  /** rfoffline://<kind>/<id>/<file in the package>, for the fonts. */
  private static registerFonts(): void {
    if (OfflineMapSources.registered) return;
    OfflineMapSources.registered = true;
    addProtocol(OfflineMapSources.protocol, async (params) => {
      const match = /^rfoffline:\/\/([a-z]+\/[a-z0-9-]+)\/(.+)$/.exec(params.url);
      const index = match ? OfflineMapSources.opened.get(match[1]) : undefined;
      if (!match || !index) throw new Error(`${params.url} is not in a saved package`);
      return { data: await index.bytes(decodeURIComponent(match[2])) };
    });
  }
}
