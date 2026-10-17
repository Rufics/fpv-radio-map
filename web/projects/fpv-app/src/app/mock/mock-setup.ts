import { EnvironmentProviders, Provider, makeEnvironmentProviders, provideAppInitializer } from '@angular/core';
import { mockApiInterceptor } from './mock-api.interceptor';
import { registerMockMapProtocols } from './mock-protocols';

/**
 * DEV MOCK wiring. Loaded by main.ts only when `environment.mockApi` is true,
 * so production bundles never contain the mock backend.
 */
export const MOCK_INTERCEPTORS = [mockApiInterceptor];

export function provideMockMaps(): EnvironmentProviders | Provider[] {
  return makeEnvironmentProviders([provideAppInitializer(() => registerMockMapProtocols())]);
}
