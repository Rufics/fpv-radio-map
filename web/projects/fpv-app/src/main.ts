import { bootstrapApplication } from '@angular/platform-browser';
import { environment } from './environments/environment';
import { appConfig } from './app/app.config';
import { App } from './app/app';
import { loadMock } from './app/mock-entry';

async function start(): Promise<void> {
  if (environment.mockApi && loadMock) {
    const mock = await loadMock();
    await bootstrapApplication(App, appConfig(mock.MOCK_INTERCEPTORS, [mock.provideMockMaps()]));
  } else {
    await bootstrapApplication(App, appConfig());
  }
}

start().catch((err) => console.error(err));
