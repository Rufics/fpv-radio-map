/**
 * DEV MOCK ONLY — an in-memory stand-in for the rfmap server that implements the contract
 * (docs/api/openapi.yaml) for the reference case. Enabled by `environment.mockApi`.
 *
 * Shared-link demo: /m/v/demo-view (PIN 246810), /m/e/demo-edit (PIN 135790), /m/v/demo-revoked.
 * Scenarios: add ?scenario=missing-elevation | module-failed | stale-forecast to the page URL.
 */
import { HttpErrorResponse, HttpHeaders, HttpParams, HttpRequest, HttpResponse } from '@angular/common/http';
import { fpvCoreMock } from './fpv-core-mock';
import { fpvAltitudeMock, fpvAltitudePrepareMock } from './fpv-altitude-mock';
import {
  CacheLimit,
  CacheTier,
  CoverageGrid,
  CreateLinkRequest,
  FlightView,
  Job,
  LIBRARY_KINDS,
  LibraryKind,
  LinkKind,
  Mission,
  MissionAltitude,
  ModuleInfo,
  RouteResult,
  SchemaValidator,
  ShareLink,
  TrustedKey,
} from '@rfmap/ui-kit';
import {
  LIBRARY,
  MockScenario,
  coverageGrid,
  elevationProfile,
  exampleMission,
  horizon,
  routeResult,
  weatherAt,
} from './reference-case';
import { baseMap, contourLines } from './mock-terrain';
import { MockFlights } from './mock-flights';

interface StoredMission {
  id: string;
  mission: Mission;
  revision: number;
  ownerKey: string;
  lastResult?: RouteResult;
  flights?: FlightView[];
  links: (ShareLink & { token: string; pin: string; wrong: number; pausedUntilMs?: number })[];
}

interface StoredJob {
  id: string;
  missionId: string | null;
  mission: Mission;
  started: number;
  cancelled: boolean;
  slow: boolean;
}

type Handler = (req: HttpRequest<unknown>, m: RegExpMatchArray) => HttpResponse<unknown> | Promise<HttpResponse<unknown>>;

const ALPHABET = 'abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789';
// Uniform over the 57 symbols: take 6 random bits (0–63) and skip the values above the alphabet.
const rnd = (n = 20) => {
  let out = '';
  const bytes = new Uint8Array(n);
  while (out.length < n) {
    for (const b of crypto.getRandomValues(bytes)) {
      const i = b >>> 2;
      if (i < ALPHABET.length && out.length < n) out += ALPHABET[i];
    }
  }
  return out;
};

export class MockBackend {
  private readonly missions = new Map<string, StoredMission>();
  private readonly sessions = new Map<string, { missionId: string; role: 'edit' | 'view' }>();
  private readonly jobs = new Map<string, StoredJob>();
  private readonly library: Record<LibraryKind, Record<string, unknown>[]> = {
    'radio-profiles': structuredClone(LIBRARY.radios) as unknown as Record<string, unknown>[],
    antennas: structuredClone(LIBRARY.antennas) as unknown as Record<string, unknown>[],
    batteries: structuredClone(LIBRARY.batteries) as unknown as Record<string, unknown>[],
    drones: structuredClone(LIBRARY.drones) as unknown as Record<string, unknown>[],
  };
  private readonly cache: CacheTier[] = [
    { id: 'dem-blocks', label: 'Elevation blocks in memory', limitBytes: 2e9, usedBytes: 1.1e9, entries: 412, hits: 96210, misses: 2980, evictions: 120, hitRate: 0.97 },
    { id: 'results-memory', label: 'Results in memory', limitBytes: 5e8, usedBytes: 1.8e8, entries: 61, hits: 830, misses: 520, evictions: 4, hitRate: 0.61 },
    { id: 'results-table', label: 'Results table (Postgres)', limitBytes: 4e9, limitRows: 50000, usedBytes: 0.9e9, entries: 8120, hits: 2210, misses: 1430, evictions: 0, hitRate: 0.61 },
  ];
  private readonly keys: TrustedKey[] = [
    { id: '9f3a1c0b', label: 'Server owner key', key: 'rfmod-ed25519 9f3a1c0b5e7d…', builtIn: true, firstParty: true },
    { id: '4be27d11', label: 'club.ham publisher', key: 'rfmod-ed25519 4be27d11a0c3…', builtIn: false, firstParty: false },
  ];
  private readonly history = new Map<string, { version: number; verified: boolean; item: Record<string, unknown>; changedBy: string; changedAt: string }[]>();
  private readonly modules: ModuleInfo[] = structuredClone(MODULES);
  private readonly validator = new SchemaValidator();
  private readonly routes: [string, RegExp, Handler][] = [];

  constructor(private readonly scenario: () => MockScenario = () => 'normal') {
    this.seed();
    const r = (method: string, re: RegExp, h: Handler) => this.routes.push([method, re, h]);
    r('GET', /^\/config\/map$/, () => this.ok(this.mapConfig()));
    r('POST', /^\/missions$/, (req) => this.createMission(req.body as Mission));
    r('GET', /^\/missions\/([^/]+)$/, (req, m) => this.getMission(req, m[1]));
    r('PUT', /^\/missions\/([^/]+)$/, (req, m) => this.putMission(req, m[1]));
    r('GET', /^\/missions\/([^/]+)\/links$/, (req, m) => this.withOwner(req, m[1], (s) => this.ok(s.links.map(publicLink))));
    r('POST', /^\/missions\/([^/]+)\/links$/, (req, m) => this.withOwner(req, m[1], (s) => this.createLink(s, req.body as CreateLinkRequest)));
    r('DELETE', /^\/missions\/([^/]+)\/links\/([^/]+)$/, (req, m) => this.withOwner(req, m[1], (s) => this.revokeLink(s, m[2])));
    r('GET', /^\/links\/([^/]+)$/, (_req, m) => this.linkInfo(m[1]));
    r('POST', /^\/links\/([^/]+)\/open$/, (req, m) => this.openLink(m[1], (req.body as { pin?: string })?.pin ?? ''));
    r('POST', /^\/calc\/route$/, (req) => this.calcRoute(req));
    r('POST', /^\/calc\/path$/, () => { throw this.error(501, 'Not part of the mock'); });
    r('POST', /^\/calc\/coverage$/, (req) => this.startJob(req));
    r('GET', /^\/jobs\/([^/]+)$/, (_req, m) => this.getJob(m[1]));
    r('DELETE', /^\/jobs\/([^/]+)$/, (_req, m) => this.cancelJob(m[1]));
    r('GET', /^\/jobs\/([^/]+)\/coverage\/(control|video)\.json$/, (_req, m) => this.coverage(m[1], m[2] as LinkKind));
    r('GET', /^\/elevation\/profile$/, (req) => this.profile(req));
    r('POST', /^\/flightlogs\/decode$/, (req) => this.decodeLog(req));
    r('GET', /^\/missions\/([^/]+)\/flightlogs$/, (req, m) => this.withSession(req, m[1], false, (s) => this.ok((s.flights ?? []).map(MockFlights.row))));
    r('POST', /^\/missions\/([^/]+)\/flightlogs$/, (req, m) => this.withSession(req, m[1], true, (s) => this.importLog(req, s)));
    r('GET', /^\/missions\/([^/]+)\/flightlogs\/([^/]+)$/, (req, m) => this.withSession(req, m[1], false, (s) => this.flightView(s, m[2])));
    r('DELETE', /^\/missions\/([^/]+)\/flightlogs\/([^/]+)$/, (req, m) => this.withSession(req, m[1], true, (s) => this.deleteLog(s, m[2])));
    r('GET', /^\/library\/([a-z-]+)$/, (_req, m) => this.ok(this.library[m[1] as LibraryKind] ?? []));
    r('POST', /^\/library\/([a-z-]+)$/, (req, m) => this.admin(req, () => this.saveLibrary(m[1] as LibraryKind, null, req.body)));
    r('GET', /^\/library\/([a-z-]+)\/([^/]+)\/history$/, (_req, m) => this.ok(this.history.get(`${m[1]}/${m[2]}`) ?? []));
    r('GET', /^\/library\/([a-z-]+)\/([^/]+)$/, (_req, m) => this.libraryItem(m[1] as LibraryKind, m[2]));
    r('PUT', /^\/library\/([a-z-]+)\/([^/]+)$/, (req, m) => this.admin(req, () => this.saveLibrary(m[1] as LibraryKind, m[2], req.body)));
    r('GET', /^\/weather\/horizon$/, () => this.ok(horizon()));
    r('GET', /^\/weather\/point$/, (req) => this.ok(weatherAt(req.params.get('timeUtc') ?? undefined, this.scenario() === 'stale-forecast', MockBackend.placeOf(req.params))));
    r('GET', /^\/modules$/, () => this.ok(this.modules));
    r('PUT', /^\/admin\/modules\/([^/]+)$/, (req, m) => this.admin(req, () => this.setModule(decodeURIComponent(m[1]), req.body as { enabled?: boolean })));
    r('POST', /^\/admin\/modules\/reload$/, (req) => this.admin(req, () => this.ok({ modules: this.modules, notices: [] })));
    r('GET', /^\/admin\/modules\/notices$/, (req) => this.admin(req, () => this.ok([])));
    r('GET', /^\/admin\/keys$/, (req) => this.admin(req, () => this.ok(this.keys)));
    r('DELETE', /^\/admin\/cache$/, (req) => this.admin(req, () => this.ok(null, 204)));
    r('GET', /^\/admin\/cache$/, (req) => this.admin(req, () => this.ok(this.cache)));
    r('PUT', /^\/admin\/cache$/, (req) => this.admin(req, () => this.putCache(req.body as CacheLimit[])));
  }

  /** Route a request under the API base. Returns null if no mock route matches. */
  /** Always settles asynchronously: a handler that throws gives a rejected promise. */
  handle(req: HttpRequest<unknown>, path: string): Promise<HttpResponse<unknown>> {
    return Promise.resolve().then(() => {
      for (const [method, re, h] of this.routes) {
        const m = re.exec(path);
        if (m && method === req.method) return h(req, m);
      }
      throw this.error(404, `No mock for ${req.method} ${path}`);
    });
  }

  /** Simulated latency per endpoint, ms. */
  latency(path: string, method: string): number {
    if (path === '/calc/route') return 1300;
    if (method === 'POST' && path.startsWith('/links/')) return 450;
    if (path.startsWith('/jobs/') && path.endsWith('.json')) return 150;
    return 120;
  }

  // ------------------------------------------------------------------
  private seed(): void {
    const id = 'm-north-ridge';
    const mission = exampleMission();
    const s: StoredMission = { id, mission, revision: 3, ownerKey: 'owner-demo-key', links: [] };
    s.lastResult = routeResult(mission, 'normal');
    s.links.push(
      { linkId: 'l-demo-view', role: 'view', label: 'Phone crew', createdAt: new Date(Date.now() - 86400e3).toISOString(), revoked: false, openCount: 7, token: 'demo-view', pin: '246810', wrong: 0 },
      { linkId: 'l-demo-edit', role: 'edit', label: 'Co-pilot', createdAt: new Date(Date.now() - 86400e3).toISOString(), revoked: false, openCount: 2, expiresAt: new Date(Date.now() + 6 * 86400e3).toISOString(), token: 'demo-edit', pin: '135790', wrong: 0 },
      { linkId: 'l-demo-expired', role: 'view', label: 'Last week', createdAt: new Date(Date.now() - 9 * 86400e3).toISOString(), expiresAt: new Date(Date.now() - 2 * 86400e3).toISOString(), revoked: false, openCount: 3, token: 'demo-expired', pin: '111111', wrong: 0 },
      { linkId: 'l-demo-revoked', role: 'view', label: 'Old link', createdAt: new Date(Date.now() - 5 * 86400e3).toISOString(), revoked: true, openCount: 12, token: 'demo-revoked', pin: '000000', wrong: 0 },
    );
    this.missions.set(id, s);
  }

  private mapConfig() {
    return {
      basemap: { geojson: baseMap() },
      satellite: { tiles: ['mocksat://{z}/{x}/{y}'], maxzoom: 15, attribution: 'Sentinel-2 cloudless (mock)' },
      dem: { tiles: ['mockdem://{z}/{x}/{y}'], encoding: 'terrarium', maxzoom: 13 },
      contours: { geojson: contourLines() },
      weather: { tiles: ['mockwx://{layer}/{time}/{z}/{x}/{y}'], maxzoom: 10 },
      attribution: ['© OpenMapTiles © OpenStreetMap contributors', 'Copernicus DEM GLO-30'],
      center: [35.21, 48.548],
      zoom: 12,
    };
  }

  private ok<T>(body: T, status = 200, headers?: HttpHeaders): HttpResponse<T> {
    return new HttpResponse({ status, body, headers });
  }

  private error(status: number, message: string, extra: Record<string, unknown> = {}, headers?: HttpHeaders): HttpErrorResponse {
    return new HttpErrorResponse({ status, statusText: message, error: { message, ...extra }, headers });
  }

  private sessionOf(req: HttpRequest<unknown>) {
    const auth = req.headers.get('Authorization') ?? '';
    return this.sessions.get(auth.replace(/^Bearer /, ''));
  }

  private admin(req: HttpRequest<unknown>, fn: () => HttpResponse<unknown> | Promise<HttpResponse<unknown>>) {
    if (!req.headers.get('Authorization')?.startsWith('Bearer ')) throw this.error(401, 'Admin token required');
    return fn();
  }

  private withOwner(req: HttpRequest<unknown>, id: string, fn: (s: StoredMission) => HttpResponse<unknown>) {
    const s = this.missions.get(id);
    if (!s) throw this.error(404, 'No such mission');
    if (req.headers.get('X-Owner-Key') !== s.ownerKey) throw this.error(403, 'Owner key required');
    return fn(s);
  }

  private withSession(req: HttpRequest<unknown>, id: string, edit: boolean, fn: (s: StoredMission) => HttpResponse<unknown>) {
    const s = this.missions.get(id);
    const sess = this.sessionOf(req);
    if (!s) throw this.error(404, 'No such mission');
    if (!sess) throw this.error(401, 'Session required');
    if (sess.missionId !== id) throw this.error(403, 'this session is for another mission');
    if (edit && sess.role !== 'edit') throw this.error(403, 'this is a read-only link');
    return fn(s);
  }

  // ---- flight logs (see mock-flights.ts: a synthetic flight along the route) ----
  private static logFile(req: HttpRequest<unknown>): { name: string; bytes: number } {
    const body = req.body as Blob | ArrayBuffer | null;
    const bytes = body instanceof Blob ? body.size : (body?.byteLength ?? 0);
    return { name: req.params.get('fileName') ?? 'flight.log', bytes };
  }

  private decodeLog(req: HttpRequest<unknown>): HttpResponse<unknown> {
    const { name, bytes } = MockBackend.logFile(req);
    if (bytes === 0) throw this.error(400, 'the request body is empty: send the log file\'s bytes');
    const mission = this.missions.get(this.sessionOf(req)?.missionId ?? '')?.mission ?? null;
    const flights = MockFlights.decode(name, bytes, mission);
    return this.ok({ decoder: 'fpv.blackbox', source: { decoder: 'fpv.blackbox', decoderVersion: '1.0.0', format: 'betaflight-blackbox', fileName: name, bytes }, flights });
  }

  private importLog(req: HttpRequest<unknown>, s: StoredMission): HttpResponse<unknown> {
    const { name, bytes } = MockBackend.logFile(req);
    const flights = MockFlights.decode(name, bytes, s.mission).map((v) => ({ ...v, id: `fl-${rnd(10)}` }));
    s.flights = [...flights, ...(s.flights ?? [])];
    return this.ok({ decoder: 'fpv.blackbox', fileName: name, flights: flights.map(MockFlights.row) }, 201);
  }

  private flightView(s: StoredMission, logId: string): HttpResponse<unknown> {
    const v = s.flights?.find((f) => f.id === logId);
    if (!v) throw this.error(404, 'no such flight log in this mission');
    return this.ok({ ...v, plan: MockFlights.planFact(v, s.mission, s.lastResult) });
  }

  private deleteLog(s: StoredMission, logId: string): HttpResponse<unknown> {
    if (!s.flights?.some((f) => f.id === logId)) throw this.error(404, 'no such flight log in this mission');
    s.flights = s.flights.filter((f) => f.id !== logId);
    return this.ok(null, 204);
  }

  private createMission(mission: Mission): HttpResponse<unknown> {
    const v = this.validator.validateMission(mission);
    if (!v.ok) throw this.error(422, 'Mission does not match mission.schema.json', { issues: v.issues });
    const id = `m-${rnd(8)}`;
    const ownerKey = `ok-${rnd(28)}`;
    const session = `s-${rnd(24)}`;
    this.missions.set(id, { id, mission: structuredClone(mission), revision: 1, ownerKey, links: [] });
    this.sessions.set(session, { missionId: id, role: 'edit' });
    return this.ok({ missionId: id, ownerKey, session, revision: 1 }, 201);
  }

  private getMission(req: HttpRequest<unknown>, id: string): HttpResponse<unknown> {
    const s = this.missions.get(id);
    const sess = this.sessionOf(req);
    if (!s) throw this.error(404, 'No such mission');
    if (sess?.missionId !== id) throw this.error(401, 'Session required');
    return this.ok({ mission: s.mission, role: sess.role, revision: s.revision, lastResult: s.lastResult });
  }

  private putMission(req: HttpRequest<unknown>, id: string): HttpResponse<unknown> {
    const s = this.missions.get(id);
    const sess = this.sessionOf(req);
    if (!s) throw this.error(404, 'No such mission');
    if (sess?.missionId !== id || sess.role !== 'edit') throw this.error(403, 'Edit session required');
    const ifMatch = Number((req.headers.get('If-Match') ?? '').replaceAll('"', ''));
    if (ifMatch !== s.revision) throw this.error(409, 'Someone saved in between');
    const v = this.validator.validateMission(req.body);
    if (!v.ok) throw this.error(422, 'Mission does not match mission.schema.json', { issues: v.issues });
    s.mission = structuredClone(req.body as Mission);
    s.revision++;
    return this.ok(null, 200, new HttpHeaders({ ETag: `"${s.revision}"` }));
  }

  private createLink(s: StoredMission, body: CreateLinkRequest): HttpResponse<unknown> {
    if (!/^\d{6}$/.test(body?.pin ?? '')) throw this.error(400, 'PIN must be 6 digits');
    if (body.expiresAt !== undefined && !isFutureDate(body.expiresAt)) throw this.error(400, 'expiresAt must be a future date-time');
    const token = rnd(32);
    const link = { linkId: `l-${rnd(6)}`, role: body.role, label: body.label, createdAt: new Date().toISOString(), revoked: false, openCount: 0, expiresAt: body.expiresAt, token, pin: body.pin, wrong: 0 };
    s.links.push(link);
    return this.ok({ ...publicLink(link), token }, 201);
  }

  private revokeLink(s: StoredMission, linkId: string): HttpResponse<unknown> {
    const link = s.links.find((l) => l.linkId === linkId);
    if (!link) throw this.error(404, 'No such link');
    link.revoked = true;
    for (const [k, v] of this.sessions) if (v.missionId === s.id && (v as { linkId?: string }).linkId === linkId) this.sessions.delete(k);
    return this.ok(null, 204);
  }

  private findLink(token: string) {
    for (const s of this.missions.values()) {
      const link = s.links.find((l) => l.token === token);
      if (link) return { s, link };
    }
    throw this.error(404, 'Unknown link');
  }

  private gone(link: StoredMission['links'][number]): void {
    if (link.revoked) throw this.error(410, 'This link was revoked by the mission owner.');
    if (link.expiresAt && Date.parse(link.expiresAt) <= Date.now()) throw this.error(410, 'This link has expired.');
  }

  private pausedIso(link: StoredMission['links'][number]): string | undefined {
    return link.pausedUntilMs && link.pausedUntilMs > Date.now() ? new Date(link.pausedUntilMs).toISOString() : undefined;
  }

  private linkInfo(token: string): HttpResponse<unknown> {
    const { s, link } = this.findLink(token);
    this.gone(link);
    return this.ok({ role: link.role, missionName: s.mission.name, expiresAt: link.expiresAt, pausedUntil: this.pausedIso(link) });
  }

  private paused(seconds: number): HttpErrorResponse {
    return this.error(429, 'Paused after too many wrong PINs', { retryAfterS: seconds }, new HttpHeaders({ 'Retry-After': String(seconds) }));
  }

  private openLink(token: string, pin: string): HttpResponse<unknown> {
    const { s, link } = this.findLink(token);
    this.gone(link);
    if (link.pausedUntilMs && link.pausedUntilMs > Date.now()) throw this.paused(Math.ceil((link.pausedUntilMs - Date.now()) / 1000));
    if (pin !== link.pin) {
      link.wrong++;
      if (link.wrong >= 5) {
        link.wrong = 0;
        link.pausedUntilMs = Date.now() + 15 * 60e3;
        throw this.paused(900);
      }
      throw this.error(401, 'Wrong PIN', { triesLeft: 5 - link.wrong });
    }
    link.wrong = 0;
    link.lastOpenedAt = new Date().toISOString();
    link.openCount = (link.openCount ?? 0) + 1;
    const session = `s-${rnd(24)}`;
    this.sessions.set(session, { missionId: s.id, role: link.role, linkId: link.linkId } as never);
    return this.ok({ session, missionId: s.id, role: link.role, missionName: s.mission.name });
  }

  private calcRoute(req: HttpRequest<unknown>): HttpResponse<unknown> {
    // route.prepare first: the legs and the modules see the automatic heights.
    const body = req.body as Mission;
    const prepared = body.modules?.['fpv.altitude'] ? fpvAltitudePrepareMock(body) : { mission: body, autoHeights: [] };
    const mission = prepared.mission;
    const result = routeResult(mission, this.scenario());
    if (prepared.autoHeights.length) result.autoHeights = prepared.autoHeights;
    for (const h of prepared.autoHeights.filter((a) => a.blocked)) {
      const name = mission.waypoints[h.index].label ?? `WP${h.index + 1}`;
      // As the module says it: the ceiling, or a link no height up to maxAglM clears.
      const why = h.limitedBy === 'ceiling' ? 'flying its legs safely takes more than the ceiling allows' : 'no height up to the highest allowed keeps the Fresnel zone clear';
      result.legs?.[h.index]?.reasons?.push({
        module: 'fpv.altitude',
        severity: 'warn',
        leg: h.index,
        text: `${name}: automatic height ${h.heightM} ${MissionAltitude.unit(mission)}, but ${why}. A relay (fpv.relay) or another launch point (fpv.best-launch) may help.`,
      });
    }
    if (mission.modules?.['fpv.core']) result.modules = { ...result.modules, 'fpv.core': fpvCoreMock(mission) };
    if (mission.modules?.['fpv.altitude']) result.modules = { ...result.modules, 'fpv.altitude': fpvAltitudeMock(mission) };
    const sess = this.sessionOf(req);
    const stored = sess ? this.missions.get(sess.missionId) : undefined;
    if (stored && sess?.role === 'edit') {
      result.missionRevision = stored.revision;
      stored.lastResult = result;
    }
    return this.ok(result);
  }

  private startJob(req: HttpRequest<unknown>): HttpResponse<unknown> {
    const sess = this.sessionOf(req);
    const id = `job-${rnd(10)}`;
    const job: StoredJob = { id, missionId: sess?.missionId ?? null, mission: structuredClone(req.body as Mission), started: Date.now(), cancelled: false, slow: false };
    this.jobs.set(id, job);
    return this.ok(this.jobView(job), 202);
  }

  /** Parts finish one after another over ~3 s; finished coverage parts can be fetched at once. */
  private jobView(job: StoredJob): Job {
    const t = (Date.now() - job.started) / 1000;
    const kinds = job.mission.evaluate === 'both' ? (['control', 'video'] as const) : ([job.mission.evaluate] as const);
    const plan: { name: string; link?: LinkKind; start: number; dur: number; cached?: boolean }[] = [
      { name: 'Elevation for 16 × 16 km', start: 0, dur: 0.1, cached: true },
      { name: `Route profiles, ${job.mission.waypoints.length} legs`, start: 0, dur: 0.9 },
      ...kinds.map((k, i) => ({ name: `Coverage grid, ${k} link`, link: k, start: 0.6 + i * 1.3, dur: 1.4 })),
    ];
    const parts = plan.map((p) => {
      const base = p.link ? { name: p.name, link: p.link } : { name: p.name };
      if (job.cancelled && t < p.start + p.dur) return { ...base, state: 'waiting' as const, progress: 0 };
      if (t >= p.start + p.dur) return { ...base, state: p.cached ? ('cached' as const) : ('done' as const), progress: 1 };
      if (t >= p.start) return { ...base, state: 'running' as const, progress: (t - p.start) / p.dur };
      return { ...base, state: 'waiting' as const, progress: 0 };
    });
    const done = parts.every((p) => p.state === 'done' || p.state === 'cached');
    return { jobId: job.id, createdAt: new Date(job.started).toISOString(), state: this.jobState(job.cancelled, done), parts };
  }

  private jobState(cancelled: boolean, done: boolean): NonNullable<Job['state']> {
    if (cancelled) return 'cancelled';
    return done ? 'done' : 'running';
  }

  private getJob(id: string): HttpResponse<unknown> {
    const job = this.jobs.get(id);
    if (!job) throw this.error(404, 'No such job');
    return this.ok(this.jobView(job));
  }

  private cancelJob(id: string): HttpResponse<unknown> {
    const job = this.jobs.get(id);
    if (job) job.cancelled = true;
    return this.ok(null, 204);
  }

  private coverage(id: string, link: LinkKind): HttpResponse<CoverageGrid> {
    const job = this.jobs.get(id);
    if (!job) throw this.error(404, 'No such job');
    return this.ok(coverageGrid(job.mission, link));
  }

  private profile(req: HttpRequest<unknown>): HttpResponse<unknown> {
    const parse = (s: string | null) => {
      const [lat, lon] = (s ?? '').split(',').map(Number);
      if (!Number.isFinite(lat) || !Number.isFinite(lon)) throw this.error(400, 'from/to must be "lat,lon"');
      return { lat, lon };
    };
    return this.ok(elevationProfile(parse(req.params.get('from')), parse(req.params.get('to'))));
  }

  private libraryItem(kind: LibraryKind, id: string): HttpResponse<unknown> {
    const item = (this.library[kind] ?? []).find((e) => e['id'] === id);
    if (!item) throw this.error(404, 'No such item');
    return this.ok(item);
  }

  /** POST adds version 1; PUT needs the edited `version` (409 if it moved on). 422 paths are JSON pointers. */
  private saveLibrary(kind: LibraryKind, id: string | null, body: unknown): HttpResponse<unknown> {
    if (!LIBRARY_KINDS.some((k) => k.kind === kind)) throw this.error(404, 'Unknown library kind');
    const entry = structuredClone(body) as Record<string, unknown>;
    const edited = entry['version'];
    delete entry['version'];
    delete entry['verified'];
    const list = this.library[kind];
    const idx = list.findIndex((e) => e['id'] === (id ?? entry['id']));
    if (id === null) {
      if (idx >= 0) throw this.error(409, `An item with id ${entry['id']} exists`);
      const added = this.validated(kind, { ...entry, version: 1, verified: false });
      list.push(added);
      return this.ok(added, 201);
    }
    if (idx < 0) throw this.error(404, 'No such item');
    const current = list[idx];
    const currentVersion = Number(current['version'] ?? 1);
    if (Number(edited) !== currentVersion) throw this.error(409, `The item changed since version ${versionLabel(edited)}; now at version ${currentVersion}`);
    const saved = this.validated(kind, { ...entry, id, version: currentVersion + 1, verified: true });
    const key = `${kind}/${id}`;
    const hist = this.history.get(key) ?? [];
    hist.unshift({ version: currentVersion, verified: current['verified'] !== false, item: current, changedBy: 'admin', changedAt: new Date().toISOString() });
    this.history.set(key, hist);
    list[idx] = saved;
    return this.ok(saved, 200);
  }

  /** The item as it will be stored (the server sets version and verified), checked against its schema. */
  private validated(kind: LibraryKind, item: Record<string, unknown>): Record<string, unknown> {
    const v = this.validator.validateLibrary(kind, item);
    if (!v.ok) throw this.error(422, 'Schema validation failed', { issues: v.issues });
    return item;
  }

  private setModule(id: string, body: { enabled?: boolean }): HttpResponse<unknown> {
    const mods = this.modules.filter((x) => x.id === id);
    if (!mods.length) throw this.error(422, 'Unknown module');
    if (typeof body?.enabled === 'boolean') for (const x of mods) {
      x.state = body.enabled ? 'active' : 'disabled';
      x.stateDetail = body.enabled ? undefined : 'Disabled by the admin';
    }
    return this.ok(this.modules);
  }

  /** The place a request names, or none when lat or lon is missing (the pilot's reference weather). */
  private static placeOf(params: HttpParams): { lat: number; lon: number } | undefined {
    const lat = Number(params.get('lat') ?? Number.NaN);
    const lon = Number(params.get('lon') ?? Number.NaN);
    return Number.isFinite(lat) && Number.isFinite(lon) ? { lat, lon } : undefined;
  }

  private putCache(body: CacheLimit[]): HttpResponse<unknown> {
    if (!Array.isArray(body)) throw this.error(400, 'Expected an array of {id, limitBytes?, limitRows?}');
    for (const b of body) {
      if (!this.cache.some((c) => c.id === b.id)) throw this.error(400, `Unknown cache tier ${b.id}`);
      const bad = (n: unknown) => n !== undefined && !(Number.isInteger(n) && (n as number) >= 0);
      if (bad(b.limitBytes) || bad(b.limitRows)) throw this.error(400, 'Limits must be integers ≥ 0');
      if (b.limitRows !== undefined && b.id !== 'results-table') throw this.error(400, 'limitRows is for results-table only');
    }
    for (const b of body) {
      const t = this.cache.find((c) => c.id === b.id)!;
      if (b.limitBytes !== undefined) t.limitBytes = b.limitBytes;
      if (b.limitRows !== undefined) t.limitRows = b.limitRows;
    }
    return this.ok(this.cache);
  }
}

/** True if `iso` parses to a time after now (an unparsable string is not in the future). */
function isFutureDate(iso: string): boolean {
  return Date.parse(iso) > Date.now();
}

/** The edited version as shown in a conflict message; '?' when the client sent none. */
function versionLabel(edited: unknown): string {
  if (edited === undefined || edited === null) return '?';
  if (typeof edited === 'string' || typeof edited === 'number' || typeof edited === 'boolean') {
    return String(edited);
  }
  return JSON.stringify(edited) ?? '?';
}

function publicLink(l: ShareLink & { token?: string; pin?: string; pausedUntilMs?: number }): ShareLink {
  const { linkId, role, label, createdAt, lastOpenedAt, revoked, openCount, expiresAt } = l;
  const pausedUntil = l.pausedUntilMs && l.pausedUntilMs > Date.now() ? new Date(l.pausedUntilMs).toISOString() : undefined;
  return { linkId, role, label, createdAt, lastOpenedAt, revoked, openCount, expiresAt, pausedUntil };
}

export const MODULES: ModuleInfo[] = [
  { id: 'rf-core', version: '1.3.0', title: 'Radio core', builtIn: true, lifecycle: 'stable', provides: ['link.budget', 'diffraction'], signedBy: 'built in', isolated: false, state: 'active' },
  { id: 'rf.weather', version: '1.1.0', title: 'Weather and refraction', lifecycle: 'stable', requires: ['rf-core>=1.2'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'rf.clutter', version: '0.9.2', title: 'Land cover clutter', lifecycle: 'beta', requires: ['rf-core>=1.2'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'fpv.core', version: '1.0.0', title: 'FPV core', lifecycle: 'beta', provides: ['route.analysis'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'fpv.battery', version: '1.0.4', title: 'Battery range', lifecycle: 'stable', requires: ['fpv.core>=1'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'rf.flightlog', version: '1.3.0', title: 'Flight log replay', builtIn: true, lifecycle: 'stable', provides: ['flightlog.processor'], signedBy: 'built in', isolated: false, state: 'active' },
  { id: 'fpv.blackbox', version: '1.0.0', title: 'Betaflight Blackbox decoder', lifecycle: 'beta', provides: ['flightlog.decoder'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'fpv.altitude', version: '1.0.0', title: 'Altitude corridor and 3D view', lifecycle: 'alpha', provides: ['route.analysis'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'fpv.min-altitude', version: '0.4.0', title: 'Minimum altitude', lifecycle: 'alpha', requires: ['fpv.core>=1'], signedBy: 'owner', isolated: true, state: 'active' },
  {
    id: 'fpv.relay', version: '1.1.0', title: 'Relay planning', lifecycle: 'deprecated', requires: ['fpv.core>=1'], signedBy: 'owner', isolated: true, state: 'active',
    deprecation: { since: 'API 1.3', removedIn: '1.6', replacement: 'fpv.relay 2.x', message: 'Use fpv.relay 2.x. 4 missions use it.' },
  },
  { id: 'fpv.relay', version: '2.0.0', title: 'Relay planning', lifecycle: 'beta', requires: ['fpv.core>=1.2'], signedBy: 'owner', isolated: true, state: 'active' },
  {
    id: 'fpv.example', version: '0.1.0', title: 'Example UI module (flight heights panel)', lifecycle: 'beta', requires: ['fpv.core>=1'], signedBy: 'owner', isolated: true, state: 'active',
    ui: { bundle: '/modules/fpv-example/ui.js', slots: ['side.panels', 'verdict.reasons'] },
  },
];
