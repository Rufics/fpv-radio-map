import { OfflineRegion } from '@rfmap/ui-kit';

/** How the offline region pages word a region: size, layers, zoom, version and date. */
export class OfflineRegionFormat {
  private static readonly layerNames: Record<string, string> = {
    basemap: 'Base map',
    satellite: 'Satellite',
    hillshade: 'Hillshade',
    terrain: '3D terrain',
    contours: 'Contours',
  };

  /** Bytes in MB below 1 GB, else in GB with one decimal: "186 MB", "1.4 GB". */
  static size(bytes: number): string {
    if (bytes >= 1e9) return `${(bytes / 1e9).toFixed(1)} GB`;
    return `${Math.max(1, Math.round(bytes / 1e6))} MB`;
  }

  static layers(r: OfflineRegion): string {
    return r.layers.map((l) => OfflineRegionFormat.layerNames[l] ?? l).join(' · ');
  }

  static zoom(r: OfflineRegion): string {
    return r.maxzoom === null ? 'Full detail' : `Up to zoom ${r.maxzoom}`;
  }

  /** The first 8 characters of the version: enough to tell versions apart on screen. */
  static version(r: OfflineRegion): string {
    return r.version.slice(0, 8);
  }

  static builtOn(r: OfflineRegion): string {
    const d = new Date(r.builtAt);
    return Number.isNaN(d.getTime())
      ? r.builtAt
      : d.toLocaleDateString('en-GB', {
          day: 'numeric',
          month: 'short',
          year: 'numeric',
          timeZone: 'UTC',
        });
  }

  /** When a package was saved on this device: "2 Nov 2026". */
  static savedOn(iso: string): string {
    const d = new Date(iso);
    return Number.isNaN(d.getTime()) ? iso : d.toLocaleDateString('en-GB', { day: 'numeric', month: 'short', year: 'numeric' });
  }

  /** km² with a precision that suits the size: "0.42 km²", "38 km²", "2,500 km²". */
  static km2(v: number): string {
    if (v < 10) return `${v.toFixed(2)} km²`;
    return `${Math.round(v).toLocaleString('en-US')} km²`;
  }

  /** The box as "48.30° N – 48.70° N, 34.90° E – 35.50° E". */
  static bbox(r: OfflineRegion): string {
    const [w, s, e, n] = r.bbox;
    const lat = (v: number) => `${Math.abs(v).toFixed(2)}° ${v < 0 ? 'S' : 'N'}`;
    const lon = (v: number) => `${Math.abs(v).toFixed(2)}° ${v < 0 ? 'W' : 'E'}`;
    return `${lat(s)} – ${lat(n)}, ${lon(w)} – ${lon(e)}`;
  }
}
