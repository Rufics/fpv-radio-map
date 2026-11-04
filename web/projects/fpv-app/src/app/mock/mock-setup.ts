import { EnvironmentProviders, Provider, makeEnvironmentProviders, provideAppInitializer } from '@angular/core';
import { OFFLINE_FETCH } from '@rfmap/ui-kit';
import { mockApiInterceptor } from './mock-api.interceptor';
import { registerMockMapProtocols } from './mock-protocols';
import { MockRegions } from './mock-regions';

/**
 * DEV MOCK wiring. Loaded by main.ts only when `environment.mockApi` is true,
 * so production bundles never contain the mock backend.
 */
export const MOCK_INTERCEPTORS = [mockApiInterceptor];

export function provideMockMaps(): EnvironmentProviders | Provider[] {
  return makeEnvironmentProviders([
    provideAppInitializer(() => registerMockMapProtocols()),
    // offline packages are fetched (streamed) outside HttpClient: the mock serves them too
    { provide: OFFLINE_FETCH, useValue: MockRegions.fetch },
  ]);
}
