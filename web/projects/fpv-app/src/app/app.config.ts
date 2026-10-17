import { ApplicationConfig, EnvironmentProviders, Provider, provideBrowserGlobalErrorListeners } from '@angular/core';
import { HttpInterceptorFn, provideHttpClient, withInterceptors } from '@angular/common/http';
import { provideRouter, withComponentInputBinding } from '@angular/router';
import { MODULE_BUNDLE_ORIGINS, RFMAP_API_BASE } from '@rfmap/ui-kit';
import { environment } from '../environments/environment';
import { routes } from './app.routes';

/** App providers. `interceptors`/`extra` carry the dev mock when it is enabled (see main.ts). */
export function appConfig(interceptors: HttpInterceptorFn[] = [], extra: (Provider | EnvironmentProviders)[] = []): ApplicationConfig {
  return {
    providers: [
      provideBrowserGlobalErrorListeners(),
      provideRouter(routes, withComponentInputBinding()),
      provideHttpClient(withInterceptors(interceptors)),
      { provide: RFMAP_API_BASE, useValue: environment.apiBase },
      // Same-origin bundles are always allowed; extra origins come from the environment.
      { provide: MODULE_BUNDLE_ORIGINS, useValue: environment.moduleBundleOrigins },
      ...extra,
    ],
  };
}
