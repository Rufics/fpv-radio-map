import { HttpErrorResponse, HttpEvent, HttpInterceptorFn } from '@angular/common/http';
import { inject } from '@angular/core';
import { RFMAP_API_BASE } from '@rfmap/ui-kit';
import { Observable, from, mergeMap, throwError, timer } from 'rxjs';
import { MockBackend } from './mock-backend';
import { MockScenario } from './reference-case';

function scenarioFromUrl(): MockScenario {
  try {
    const s = new URLSearchParams(location.search).get('scenario');
    if (s === 'missing-elevation' || s === 'module-failed' || s === 'stale-forecast') {
      sessionStorage.setItem('rfmap.mockScenario', s);
      return s;
    }
    if (s === 'normal') sessionStorage.removeItem('rfmap.mockScenario');
    return (sessionStorage.getItem('rfmap.mockScenario') as MockScenario | null) ?? 'normal';
  } catch {
    return 'normal';
  }
}

let backend: MockBackend | null = null;
export function mockBackend(): MockBackend {
  backend ??= new MockBackend(scenarioFromUrl);
  return backend;
}

/**
 * DEV MOCK: answers every request under the API base from an in-memory backend
 * (with realistic latency), so the whole flow works without the server.
 */
export const mockApiInterceptor: HttpInterceptorFn = (req, next) => {
  const base = inject(RFMAP_API_BASE);
  const url = new URL(req.url, location.origin);
  if (!url.pathname.startsWith(base)) return next(req);
  const path = url.pathname.slice(base.length) || '/';
  const be = mockBackend();
  return timer(be.latency(path, req.method)).pipe(
    mergeMap(
      () =>
        from(be.handle(req, path)).pipe(
          mergeMap((res) => {
            if (res.status >= 400) return throwError(() => new HttpErrorResponse({ status: res.status, error: res.body, headers: res.headers, url: req.url }));
            return [res.clone({ url: req.url }) as HttpEvent<unknown>];
          }),
        ) as Observable<HttpEvent<unknown>>,
    ),
  );
};
