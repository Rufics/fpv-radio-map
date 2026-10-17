/** Optimised build with the dev mock (demos, screenshots). */
export const environment = {
  production: true,
  /** Serve the reference case from the in-browser dev mock instead of the server. */
  mockApi: true,
  apiBase: '/api/v1',
  /** Used when GET /config/map is not available. PMTiles archives served by our server. */
  mapFallback: {
    // Output names of tools/tiles-builder, served under /tiles by the server.
    basemap: { pmtiles: '/tiles/basemap.pmtiles', attribution: '© OpenMapTiles © OpenStreetMap contributors' },
    satellite: { pmtiles: '/tiles/satellite.pmtiles' },
    hillshade: { pmtiles: '/tiles/hillshade.pmtiles', maxzoom: 12 },
    contours: { pmtiles: '/tiles/contours.pmtiles', sourceLayer: 'contours' },
    weather: { tiles: ['/tiles/weather/{layer}/{time}/{z}/{x}/{y}.png'] },
    attribution: [
      '© OpenMapTiles © OpenStreetMap contributors',
      'Copernicus DEM GLO-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved',
    ],
    glyphs: '/tiles/fonts/{fontstack}/{range}.pbf',
  },
  /** Extra origins module UI bundles may come from (same origin is always allowed). */
  moduleBundleOrigins: [] as string[],
};
