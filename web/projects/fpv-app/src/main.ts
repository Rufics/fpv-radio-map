import { bootstrapApplication } from '@angular/platform-browser';
import { setWorkerUrl } from 'maplibre-gl';
import { environment } from './environments/environment';
import { appConfig } from './app/app.config';
import { App } from './app/app';
import { loadMock } from './app/mock-entry';

// MapLibre 6 loads its worker as a separate module; angular.json copies it to maplibre/.
setWorkerUrl(new URL('maplibre/maplibre-gl-worker.mjs', document.baseURI).href);

async function start(): Promise<void> {
  if (environment.mockApi && loadMock) {
    const mock = await loadMock();
    await bootstrapApplication(App, appConfig(mock.MOCK_INTERCEPTORS, [mock.provideMockMaps()]));
  } else {
    await bootstrapApplication(App, appConfig());
  }
}

start().catch((err) => console.error(err));
