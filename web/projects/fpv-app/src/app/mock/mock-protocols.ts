import { addProtocol } from 'maplibre-gl';
import { demTile, satelliteTile, weatherTile } from './mock-terrain';

let done = false;

/** DEV MOCK: synthetic DEM, satellite and weather tiles generated in the browser. */
export function registerMockMapProtocols(): void {
  if (done) return;
  done = true;
  addProtocol('mockdem', async (params) => ({ data: await demTile(params.url) }));
  addProtocol('mocksat', async (params) => ({ data: await satelliteTile(params.url) }));
  addProtocol('mockwx', async (params) => ({ data: await weatherTile(params.url) }));
}
