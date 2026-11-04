import {
  AreaShape,
  OfflineAreaLimits,
  OfflineAreaRequest,
  OfflineAreaStatus,
  OfflineRegion,
  OfflineRegionAdmin,
  Sha256,
  StoredZipWriter,
} from '@rfmap/ui-kit';
import type { Polygon } from 'geojson';

/**
 * A package as the mock serves it: a real zip of stored entries, small, so saving it on the
 * device and reading it back works as with the server's.
 */
class MockPackage {
  readonly bytes: Uint8Array;

  constructor(id: string, name: string, bbox: OfflineRegion['bbox'], layers: string[]) {
    this.bytes = new StoredZipWriter()
      .add('manifest.json', JSON.stringify({ format: 'rfmap-region', formatVersion: 1, id, name, bbox, layers }))
      .add('style.json', JSON.stringify({ version: 8, name, sources: {}, layers: [], glyphs: 'glyphs/{fontstack}/{range}.pbf' }))
      .add('glyphs/Noto Sans Regular/0-255.pbf', `glyphs of ${name}`)
      .bytes();
  }

  get size(): number {
    return this.bytes.length;
  }

  get version(): string {
    return Sha256.hex(this.bytes.subarray(0, 200));
  }

  get sha256(): string {
    return Sha256.hex(this.bytes);
  }
}

/** The offline regions the dev mock offers: the reference area, an oblast and a large satellite region. */
export class MockRegions {
  private static readonly packages = new Map<string, Uint8Array>();

  static readonly list: OfflineRegion[] = [
    MockRegions.region('dnipro-ridge', 'Dnipro ridge (reference area)', [34.9, 48.3, 35.5, 48.7], ['basemap', 'hillshade', 'terrain', 'contours'], null, 2),
    MockRegions.region('dnipro-oblast', 'Dnipro oblast', [33.2, 47.4, 36.9, 49.2], ['basemap', 'hillshade', 'terrain', 'contours'], 14, 3),
    MockRegions.region('ukraine-east', 'Eastern Ukraine, satellite', [34.0, 46.8, 40.2, 50.4], ['basemap', 'satellite', 'hillshade', 'terrain'], 13, 4),
  ];

  /** GET /admin/regions: the last region is being rebuilt and keeps its package meanwhile. */
  static admin(): OfflineRegionAdmin {
    return {
      enabled: true,
      scans: 3,
      regions: MockRegions.list.map((r, i) => ({
        id: r.id,
        state: i === 2 ? 'building' : 'ready',
        error: null,
        package: r,
      })),
      areas: { enabled: true, pending: MockAreas.pending() },
    };
  }

  /** The package for a catalog URL, as fetch answers it; 404 for any other URL. */
  static readonly fetch: typeof fetch = (input) => {
    const path = /\/(regions|areas)\/[a-z0-9-]+\/packages\/[0-9a-f]{64}\.zip$/.exec(MockRegions.urlOf(input))?.[0] ?? '';
    const bytes = MockRegions.packages.get(path);
    return Promise.resolve(
      bytes ? new Response(bytes as BlobPart, { headers: { 'Content-Type': 'application/zip' } }) : new Response(null, { status: 404 }),
    );
  };

  private static urlOf(input: string | URL | Request): string {
    if (typeof input === 'string') return input;
    return input instanceof URL ? input.href : input.url;
  }

  /** A package whose zip the mock fetch serves. */
  static described(
    collection: 'regions' | 'areas',
    id: string,
    name: string,
    bbox: OfflineRegion['bbox'],
    layers: string[],
    maxzoom: number | null,
    seed: number,
  ): OfflineRegion {
    const p = new MockPackage(id, name, bbox, layers);
    const path = `/${collection}/${id}/packages/${p.version}.zip`;
    MockRegions.packages.set(path, p.bytes);
    return {
      id,
      name,
      bbox,
      maxzoom,
      layers,
      size: p.size,
      version: p.version,
      sha256: p.sha256,
      builtAt: '2026-10-30T04:12:00Z',
      preview: MockRegions.preview(seed),
      package: `/api/v1${path}`,
    };
  }

  private static region(id: string, name: string, bbox: OfflineRegion['bbox'], layers: string[], maxzoom: number | null, seed: number): OfflineRegion {
    return MockRegions.described('regions', id, name, bbox, layers, maxzoom, seed);
  }

  /** A hillshade-like picture drawn in SVG, so the mock needs no image files. */
  private static preview(seed: number): string {
    const ridges = Array.from({ length: 7 }, (_, i) => {
      const y = 20 + i * 24;
      const a = 10 + ((seed * (i + 3)) % 23);
      return `<path d="M0 ${y} C 60 ${y - a}, 120 ${y + a}, 256 ${y - a / 2}" stroke="#9aa7b4" stroke-opacity="${0.18 + i * 0.05}" stroke-width="${10 + (i % 3) * 6}" fill="none"/>`;
    }).join('');
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="171" viewBox="0 0 256 171"><rect width="256" height="171" fill="#3a4450"/>${ridges}</svg>`;
    return `data:image/svg+xml;utf8,${encodeURIComponent(svg)}`;
  }
}

/** Areas drawn in the dev mock: queued for a moment, then built, as the server does. */
export class MockAreas {
  static readonly limits: OfflineAreaLimits = {
    enabled: true,
    maxAreaKm2: 2500,
    maxVertices: 1000,
    maxzoom: 14,
    layers: ['basemap', 'satellite', 'hillshade', 'terrain', 'contours'],
  };
  static readonly queuedMs = 1500;
  static readonly buildMs = 3000;
  private static readonly asked = new Map<string, { at: number; request: OfflineAreaRequest }>();

  /** POST /areas: the status, or the reason it is refused (422). */
  static request(body: OfflineAreaRequest, now = Date.now()): OfflineAreaStatus | string {
    const ring = body?.geometry?.type === 'Polygon' ? body.geometry.coordinates[0] : null;
    if (!ring || ring.length < 4) return 'send {"geometry": a GeoJSON Polygon or MultiPolygon}';
    const km2 = new AreaShape(ring.slice(0, -1).map(([lon, lat]) => ({ lat, lon }))).areaKm2();
    if (km2 > MockAreas.limits.maxAreaKm2) {
      return `the area is ${Math.round(km2)} km², more than the ${MockAreas.limits.maxAreaKm2} km² this server builds`;
    }
    const id = `area-${Sha256.hex(JSON.stringify({ ...body, name: body.name ?? 'Drawn area' })).slice(0, 24)}`;
    if (!MockAreas.asked.has(id)) MockAreas.asked.set(id, { at: now, request: body });
    return MockAreas.status(id, now) as OfflineAreaStatus;
  }

  /** GET /areas/{id}: null for an area never asked for. */
  static status(id: string, now = Date.now()): OfflineAreaStatus | null {
    const a = MockAreas.asked.get(id);
    if (!a) return null;
    const age = now - a.at;
    if (age < MockAreas.queuedMs) return { id, state: 'queued', position: 1, error: null, package: null };
    if (age < MockAreas.queuedMs + MockAreas.buildMs) return { id, state: 'building', position: null, error: null, package: null };
    const ring = (a.request.geometry as Polygon).coordinates[0];
    const lons = ring.map((p) => p[0]);
    const lats = ring.map((p) => p[1]);
    const bbox: OfflineRegion['bbox'] = [Math.min(...lons), Math.min(...lats), Math.max(...lons), Math.max(...lats)];
    const layers = a.request.layers ?? ['basemap', 'hillshade', 'terrain', 'contours'];
    const pkg = MockRegions.described('areas', id, a.request.name ?? 'Drawn area', bbox, layers, a.request.maxzoom ?? 14, 5);
    return { id, state: 'ready', position: null, error: null, package: pkg };
  }

  static pending(now = Date.now()): number {
    return [...MockAreas.asked.keys()].filter((id) => MockAreas.status(id, now)?.state !== 'ready').length;
  }
}
