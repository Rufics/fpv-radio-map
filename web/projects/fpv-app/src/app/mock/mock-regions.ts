import { OfflineRegion, OfflineRegionAdmin } from '@rfmap/ui-kit';

/** The offline regions the dev mock offers: the reference area, an oblast and a large satellite region. */
export class MockRegions {
  private static readonly versions = [
    '48456891effc646cc6e52eb36497b80fc221d9afe4acdf103579b7d40498361f',
    '9344147c9069987c9ce242835d3a98c293315f4d77a4763ddc29c895aa18a961',
    'c0ffee5a1e2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5',
  ];

  static readonly list: OfflineRegion[] = [
    MockRegions.region(
      0,
      'dnipro-ridge',
      'Dnipro ridge (reference area)',
      [34.9, 48.3, 35.5, 48.7],
      186_400_000,
      ['basemap', 'hillshade', 'terrain', 'contours'],
      null,
    ),
    MockRegions.region(
      1,
      'dnipro-oblast',
      'Dnipro oblast',
      [33.2, 47.4, 36.9, 49.2],
      1_420_000_000,
      ['basemap', 'hillshade', 'terrain', 'contours'],
      14,
    ),
    MockRegions.region(
      2,
      'ukraine-east',
      'Eastern Ukraine, satellite',
      [34.0, 46.8, 40.2, 50.4],
      6_850_000_000,
      ['basemap', 'satellite', 'hillshade', 'terrain'],
      13,
    ),
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
    };
  }

  private static region(
    n: number,
    id: string,
    name: string,
    bbox: OfflineRegion['bbox'],
    size: number,
    layers: string[],
    maxzoom: number | null,
  ): OfflineRegion {
    const version = MockRegions.versions[n];
    return {
      id,
      name,
      bbox,
      maxzoom,
      layers,
      size,
      version,
      sha256: [...version].reverse().join(''),
      builtAt: '2026-10-30T04:12:00Z',
      preview: MockRegions.preview(n + 2),
      package: `/api/v1/regions/${id}/packages/${version}.zip`,
    };
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
