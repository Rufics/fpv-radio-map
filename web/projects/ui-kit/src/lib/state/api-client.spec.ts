import { HttpErrorResponse, HttpHeaders, provideHttpClient } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { firstValueFrom } from 'rxjs';
import { Mission } from '../models/mission.models';
import { ApiError, RFMAP_API_BASE, RfmapApiClient } from './api-client';
import { CredentialStore } from './credential-store';

const mission: Mission = {
  schema: 'mission/v1',
  name: 'Test',
  time: { mode: 'now' },
  pilot: { lat: 48.5, lon: 35.2, aglM: 1.5 },
  waypoints: [{ lat: 48.51, lon: 35.21, aglM: 80 }],
  links: {},
  evaluate: 'both',
};

/** One API method, the request it must send and the credential header it must carry. */
interface Case {
  name: string;
  call: (api: RfmapApiClient) => Promise<unknown>;
  method: string;
  url: string;
  auth?: string;
  owner?: string;
}

const cases: Case[] = [
  { name: 'createMission', call: (a) => a.createMission(mission), method: 'POST', url: '/api/x/missions' },
  { name: 'getMission', call: (a) => a.getMission('m 1'), method: 'GET', url: '/api/x/missions/m%201', auth: 'Bearer s1' },
  { name: 'listLinks', call: (a) => a.listLinks('m 1'), method: 'GET', url: '/api/x/missions/m%201/links', owner: 'k1' },
  { name: 'revokeLink', call: (a) => a.revokeLink('m 1', 'l/2', 'given'), method: 'DELETE', url: '/api/x/missions/m%201/links/l%2F2', owner: 'given' },
  { name: 'linkInfo', call: (a) => a.linkInfo('t'), method: 'GET', url: '/api/x/links/t' },
  { name: 'openLink', call: (a) => a.openLink('t', '123456'), method: 'POST', url: '/api/x/links/t/open' },
  { name: 'calcPath', call: (a) => a.calcPath('m 1', {} as never), method: 'POST', url: '/api/x/calc/path', auth: 'Bearer s1' },
  { name: 'startCoverage', call: (a) => a.startCoverage(null, mission), method: 'POST', url: '/api/x/calc/coverage' },
  { name: 'getJob', call: (a) => a.getJob('m 1', 'j1'), method: 'GET', url: '/api/x/jobs/j1', auth: 'Bearer s1' },
  { name: 'cancelJob', call: (a) => a.cancelJob('m 1', 'j1'), method: 'DELETE', url: '/api/x/jobs/j1', auth: 'Bearer s1' },
  { name: 'getCoverage', call: (a) => a.getCoverage('m 1', 'j1', 'video'), method: 'GET', url: '/api/x/jobs/j1/coverage/video.json', auth: 'Bearer s1' },
  { name: 'elevationProfile', call: (a) => a.elevationProfile({ lat: 1, lon: 2 }, { lat: 3, lon: 4 }), method: 'GET', url: '/api/x/elevation/profile?from=1,2&to=3,4' },
  { name: 'library', call: (a) => a.library('antennas'), method: 'GET', url: '/api/x/library/antennas' },
  { name: 'libraryItem', call: (a) => a.libraryItem('antennas', 'a b'), method: 'GET', url: '/api/x/library/antennas/a%20b' },
  { name: 'libraryHistory', call: (a) => a.libraryHistory('antennas', 'a'), method: 'GET', url: '/api/x/library/antennas/a/history' },
  { name: 'addLibraryEntry', call: (a) => a.addLibraryEntry('drones', {}), method: 'POST', url: '/api/x/library/drones', auth: 'Bearer admin' },
  { name: 'saveLibraryEntry', call: (a) => a.saveLibraryEntry('drones', 'd', {}), method: 'PUT', url: '/api/x/library/drones/d', auth: 'Bearer admin' },
  { name: 'weatherPoint', call: (a) => a.weatherPoint(1, 2, '2026-10-18T12:00:00Z'), method: 'GET', url: '/api/x/weather/point?lat=1&lon=2&timeUtc=2026-10-18T12:00:00Z' },
  { name: 'weatherHorizon', call: (a) => a.weatherHorizon(), method: 'GET', url: '/api/x/weather/horizon' },
  { name: 'modules', call: (a) => a.modules(), method: 'GET', url: '/api/x/modules' },
  { name: 'setModule', call: (a) => a.setModule('fpv.relay', { enabled: true }), method: 'PUT', url: '/api/x/admin/modules/fpv.relay', auth: 'Bearer admin' },
  { name: 'reloadModules', call: (a) => a.reloadModules(), method: 'POST', url: '/api/x/admin/modules/reload', auth: 'Bearer admin' },
  { name: 'moduleNotices', call: (a) => a.moduleNotices(), method: 'GET', url: '/api/x/admin/modules/notices', auth: 'Bearer admin' },
  { name: 'adminKeys', call: (a) => a.adminKeys(), method: 'GET', url: '/api/x/admin/keys', auth: 'Bearer admin' },
  { name: 'adminCache', call: (a) => a.adminCache(), method: 'GET', url: '/api/x/admin/cache', auth: 'Bearer admin' },
  { name: 'putAdminCache', call: (a) => a.putAdminCache([]), method: 'PUT', url: '/api/x/admin/cache', auth: 'Bearer admin' },
  { name: 'clearCache', call: (a) => a.clearCache(), method: 'DELETE', url: '/api/x/admin/cache?tier=all', auth: 'Bearer admin' },
  { name: 'decodeFlightLog', call: (a) => a.decodeFlightLog(null, new Blob(['x']), 'a b.bbl'), method: 'POST', url: '/api/x/flightlogs/decode?fileName=a%20b.bbl' },
  { name: 'decodeFlightLog in a mission', call: (a) => a.decodeFlightLog('m 1', new Blob(['x']), 'a.bbl'), method: 'POST', url: '/api/x/flightlogs/decode?fileName=a.bbl', auth: 'Bearer s1' },
  { name: 'importFlightLog', call: (a) => a.importFlightLog('m 1', new Blob(['x']), 'a.bbl'), method: 'POST', url: '/api/x/missions/m%201/flightlogs?fileName=a.bbl', auth: 'Bearer s1' },
  { name: 'listFlightLogs', call: (a) => a.listFlightLogs('m 1'), method: 'GET', url: '/api/x/missions/m%201/flightlogs', auth: 'Bearer s1' },
  { name: 'getFlightLog', call: (a) => a.getFlightLog('m 1', 'f/1'), method: 'GET', url: '/api/x/missions/m%201/flightlogs/f%2F1', auth: 'Bearer s1' },
  { name: 'deleteFlightLog', call: (a) => a.deleteFlightLog('m 1', 'f1'), method: 'DELETE', url: '/api/x/missions/m%201/flightlogs/f1', auth: 'Bearer s1' },
  { name: 'mapConfig', call: (a) => a.mapConfig(), method: 'GET', url: '/api/x/config/map' },
];

describe('RfmapApiClient', () => {
  let api: RfmapApiClient;
  let http: HttpTestingController;
  let creds: CredentialStore;

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [provideHttpClient(), provideHttpClientTesting(), { provide: RFMAP_API_BASE, useValue: '/api/x' }],
    });
    api = TestBed.inject(RfmapApiClient);
    http = TestBed.inject(HttpTestingController);
    creds = TestBed.inject(CredentialStore);
    creds.setSession('m 1', 's1');
    creds.setOwnerKey('m 1', 'k1');
    creds.setAdminToken('admin');
  });

  afterEach(() => {
    http.verify();
    creds.setSession('m 1', null);
    creds.setOwnerKey('m 1', null);
    creds.setAdminToken(null);
  });

  for (const c of cases) {
    it(`${c.name} sends ${c.method} ${c.url}`, async () => {
      const done = c.call(api);
      const req = http.expectOne((r) => r.urlWithParams === c.url);
      expect(req.request.method).toBe(c.method);
      expect(req.request.headers.get('Authorization')).toBe(c.auth ?? null);
      expect(req.request.headers.get('X-Owner-Key')).toBe(c.owner ?? null);
      req.flush({ ok: true });
      await done;
    });
  }

  it('uploads a flight log as raw bytes', async () => {
    const done = api.importFlightLog('m 1', new Blob(['abc']), 'a.bbl');
    const req = http.expectOne((r) => r.url === '/api/x/missions/m%201/flightlogs');
    expect(req.request.headers.get('Content-Type')).toBe('application/octet-stream');
    expect(req.request.body).toBeInstanceOf(Blob);
    req.flush({ flights: [] });
    expect(await done).toEqual({ flights: [] });
  });

  it('saves with If-Match and reads the new revision from the ETag', async () => {
    const saved = api.saveMission('m 1', mission, 3);
    const req = http.expectOne('/api/x/missions/m%201');
    expect(req.request.headers.get('If-Match')).toBe('"3"');
    expect(req.request.headers.get('Authorization')).toBe('Bearer s1');
    req.flush(null, { headers: new HttpHeaders({ ETag: 'W/"9"' }) });
    expect(await saved).toBe(9);

    const noTag = api.saveMission('m 1', mission, 3);
    http.expectOne('/api/x/missions/m%201').flush(null);
    expect(await noTag).toBe(4);
  });

  it('checks the PIN before asking the server', async () => {
    await expect(api.createLink('m 1', { role: 'view', pin: '12ab' })).rejects.toMatchObject({ status: 400 });
    const created = api.createLink('m 1', { role: 'view', pin: '123456' });
    const req = http.expectOne('/api/x/missions/m%201/links');
    expect(req.request.headers.get('X-Owner-Key')).toBe('k1');
    req.flush({ linkId: 'l1' });
    expect(await created).toEqual({ linkId: 'l1' });
  });

  it('sends no credentials it does not hold', async () => {
    creds.setAdminToken(null);
    const route = firstValueFrom(api.calcRoute('other', mission));
    const routeReq = http.expectOne('/api/x/calc/route');
    expect(routeReq.request.headers.has('Authorization')).toBe(false);
    routeReq.flush({ legs: [] });
    await route;

    const keys = api.adminKeys();
    const keysReq = http.expectOne('/api/x/admin/keys');
    expect(keysReq.request.headers.has('Authorization')).toBe(false);
    keysReq.flush([]);
    await keys;
  });

  it('turns HTTP errors into ApiError with the server message and retry time', async () => {
    const paused = api.openLink('t', '123456');
    http.expectOne('/api/x/links/t/open').flush({ message: 'Paused', retryAfterS: 30 }, { status: 429, statusText: 'Too Many' });
    await expect(paused).rejects.toMatchObject({ status: 429, message: 'Paused', retryAfterS: 30 });

    const busy = api.mapConfig();
    http
      .expectOne('/api/x/config/map')
      .flush('busy', { status: 503, statusText: 'Unavailable', headers: new HttpHeaders({ 'Retry-After': '5' }) });
    await expect(busy).rejects.toMatchObject({ status: 503, retryAfterS: 5 });
  });
});

describe('ApiError', () => {
  it('wraps anything thrown', () => {
    const same = new ApiError(418, 'tea');
    expect(ApiError.from(same)).toBe(same);
    expect(ApiError.from(new Error('boom'))).toMatchObject({ status: 0, message: 'boom' });
    expect(ApiError.from('text')).toMatchObject({ status: 0, message: 'text' });
    const plain = ApiError.from(new HttpErrorResponse({ status: 500, error: 'oops', url: '/x' }));
    expect(plain.status).toBe(500);
    expect(plain.retryAfterS).toBeUndefined();
  });

  it('exposes the missing-data body and the schema issues of a 422 only', () => {
    const missing = new ApiError(422, 'missing', { status: 'missing-data', spans: [] });
    expect(missing.missingData).toEqual({ status: 'missing-data', spans: [] });
    expect(new ApiError(500, 'x', { status: 'missing-data' }).missingData).toBeNull();
    expect(new ApiError(422, 'x', { other: true }).missingData).toBeNull();

    const report = new ApiError(422, 'bad', { issues: [{ path: '/pilot', message: 'required' }, {}] });
    expect(report.issues).toEqual([
      { path: '/pilot', message: 'required' },
      { path: '', message: '' },
    ]);
    expect(new ApiError(400, 'bad', { issues: [] }).issues).toEqual([]);
  });
});
