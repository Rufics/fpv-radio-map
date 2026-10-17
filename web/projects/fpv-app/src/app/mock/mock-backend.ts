/**
 * DEV MOCK ONLY — an in-memory stand-in for the rfmap server that implements the contract
 * (docs/api/openapi.yaml) for the reference case. Enabled by `environment.mockApi`.
 *
 * Shared-link demo: /m/v/demo-view (PIN 246810), /m/e/demo-edit (PIN 135790), /m/v/demo-revoked.
 * Scenarios: add ?scenario=missing-elevation | module-failed | stale-forecast to the page URL.
 */
import { HttpErrorResponse, HttpHeaders, HttpRequest, HttpResponse } from '@angular/common/http';
import { fpvCoreMock } from './fpv-core-mock';
import {
  CacheTier,
  CoverageGrid,
  CreateLinkRequest,
  Job,
  LIBRARY_KINDS,
  LibraryKind,
  LinkKind,
  Mission,
  ModuleInfo,
  RouteResult,
  SchemaValidator,
  ShareLink,
  ValidationReport,
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

interface StoredMission {
  id: string;
  mission: Mission;
  revision: number;
  ownerKey: string;
  lastResult?: RouteResult;
  links: (ShareLink & { token: string; pin: string; wrong: number; pausedUntil?: number })[];
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

const rnd = (n = 20) => Array.from(crypto.getRandomValues(new Uint8Array(n)), (b) => 'abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789'[b % 57]).join('');

export class MockBackend {
  private missions = new Map<string, StoredMission>();
  private sessions = new Map<string, { missionId: string; role: 'edit' | 'view' }>();
  private jobs = new Map<string, StoredJob>();
  private library: Record<LibraryKind, Record<string, unknown>[]> = {
    'radio-profiles': structuredClone(LIBRARY.radios) as unknown as Record<string, unknown>[],
    antennas: structuredClone(LIBRARY.antennas) as unknown as Record<string, unknown>[],
    batteries: structuredClone(LIBRARY.batteries) as unknown as Record<string, unknown>[],
    drones: structuredClone(LIBRARY.drones) as unknown as Record<string, unknown>[],
  };
  private cache: CacheTier[] = [
    { id: 'tiles', label: 'Map tiles (PMTiles range cache)', limitBytes: 20e9, usedBytes: 6.4e9, hitRate: 0.93 },
    { id: 'dem', label: 'Elevation tiles for maths', limitBytes: 8e9, usedBytes: 2.1e9, hitRate: 0.97 },
    { id: 'results', label: 'Calculation results', limitBytes: 4e9, usedBytes: 0.9e9, hitRate: 0.61 },
    { id: 'weather', label: 'Weather forecast runs', limitBytes: 2e9, usedBytes: 1.3e9, hitRate: 0.88 },
  ];
  private validator = new SchemaValidator();
  private routes: [string, RegExp, Handler][] = [];

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
    r('POST', /^\/links\/([^/]+)\/open$/, (req, m) => this.openLink(m[1], (req.body as { pin?: string })?.pin ?? ''));
    r('POST', /^\/calc\/route$/, (req) => this.calcRoute(req));
    r('POST', /^\/calc\/path$/, () => { throw this.error(501, 'Not part of the mock'); });
    r('POST', /^\/calc\/coverage$/, (req) => this.startJob(req));
    r('GET', /^\/jobs\/([^/]+)$/, (_req, m) => this.getJob(m[1]));
    r('DELETE', /^\/jobs\/([^/]+)$/, (_req, m) => this.cancelJob(m[1]));
    r('GET', /^\/jobs\/([^/]+)\/coverage\/(control|video)\.json$/, (_req, m) => this.coverage(m[1], m[2] as LinkKind));
    r('GET', /^\/elevation\/profile$/, (req) => this.profile(req));
    r('GET', /^\/library\/([a-z-]+)$/, (_req, m) => this.ok(this.library[m[1] as LibraryKind] ?? []));
    r('POST', /^\/library\/([a-z-]+)$/, (req, m) => this.admin(req, () => this.saveLibrary(m[1] as LibraryKind, null, req.body)));
    r('PUT', /^\/library\/([a-z-]+)\/([^/]+)$/, (req, m) => this.admin(req, () => this.saveLibrary(m[1] as LibraryKind, m[2], req.body)));
    r('GET', /^\/weather\/horizon$/, () => this.ok(horizon()));
    r('GET', /^\/weather\/point$/, (req) => this.ok(weatherAt(req.params.get('timeUtc') ?? undefined, this.scenario() === 'stale-forecast')));
    r('GET', /^\/modules$/, () => this.ok(MODULES));
    r('POST', /^\/admin\/modules$/, (req) => this.admin(req, () => this.uploadModule(req)));
    r('GET', /^\/admin\/keys$/, (req) => this.admin(req, () => this.ok([{ id: 'owner', name: 'Server owner key (built in)', removable: false }, { id: 'club-ham', name: 'club.ham publisher', removable: true }])));
    r('GET', /^\/admin\/cache$/, (req) => this.admin(req, () => this.ok(this.cache)));
    r('PUT', /^\/admin\/cache$/, (req) => this.admin(req, () => this.putCache(req.body as Pick<CacheTier, 'id' | 'limitBytes'>[])));
  }

  /** Route a request under the API base. Returns null if no mock route matches. */
  async handle(req: HttpRequest<unknown>, path: string): Promise<HttpResponse<unknown>> {
    for (const [method, re, h] of this.routes) {
      const m = path.match(re);
      if (m && method === req.method) return h(req, m);
    }
    throw this.error(404, `No mock for ${req.method} ${path}`);
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
    s.links.push({ linkId: 'l-demo-view', role: 'view', label: 'Phone crew', createdAt: new Date(Date.now() - 86400e3).toISOString(), revoked: false, token: 'demo-view', pin: '246810', wrong: 0 });
    s.links.push({ linkId: 'l-demo-edit', role: 'edit', label: 'Co-pilot', createdAt: new Date(Date.now() - 86400e3).toISOString(), revoked: false, token: 'demo-edit', pin: '135790', wrong: 0 });
    s.links.push({ linkId: 'l-demo-revoked', role: 'view', label: 'Old link', createdAt: new Date(Date.now() - 5 * 86400e3).toISOString(), revoked: true, token: 'demo-revoked', pin: '000000', wrong: 0 });
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

  private createMission(mission: Mission): HttpResponse<unknown> {
    const v = this.validator.validateMission(mission);
    if (!v.ok) throw this.error(422, 'Mission does not match mission.schema.json', { issues: v.issues });
    const id = `m-${rnd(8)}`;
    const ownerKey = `ok-${rnd(28)}`;
    const session = `s-${rnd(24)}`;
    this.missions.set(id, { id, mission: structuredClone(mission), revision: 1, ownerKey, links: [] });
    this.sessions.set(session, { missionId: id, role: 'edit' });
    return this.ok({ missionId: id, ownerKey, session }, 201);
  }

  private getMission(req: HttpRequest<unknown>, id: string): HttpResponse<unknown> {
    const s = this.missions.get(id);
    const sess = this.sessionOf(req);
    if (!s) throw this.error(404, 'No such mission');
    if (!sess || sess.missionId !== id) throw this.error(401, 'Session required');
    return this.ok({ mission: s.mission, role: sess.role, revision: s.revision, lastResult: s.lastResult });
  }

  private putMission(req: HttpRequest<unknown>, id: string): HttpResponse<unknown> {
    const s = this.missions.get(id);
    const sess = this.sessionOf(req);
    if (!s) throw this.error(404, 'No such mission');
    if (!sess || sess.missionId !== id || sess.role !== 'edit') throw this.error(403, 'Edit session required');
    const ifMatch = Number((req.headers.get('If-Match') ?? '').replace(/"/g, ''));
    if (ifMatch !== s.revision) throw this.error(409, 'Someone saved in between');
    const v = this.validator.validateMission(req.body);
    if (!v.ok) throw this.error(422, 'Mission does not match mission.schema.json', { issues: v.issues });
    s.mission = structuredClone(req.body as Mission);
    s.revision++;
    return this.ok(null, 200, new HttpHeaders({ ETag: `"${s.revision}"` }));
  }

  private createLink(s: StoredMission, body: CreateLinkRequest): HttpResponse<unknown> {
    if (!/^[0-9]{6}$/.test(body?.pin ?? '')) throw this.error(400, 'PIN must be 6 digits');
    const token = rnd(32);
    const link = { linkId: `l-${rnd(6)}`, role: body.role, label: body.label, createdAt: new Date().toISOString(), revoked: false, token, pin: body.pin, wrong: 0 };
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

  private openLink(token: string, pin: string): HttpResponse<unknown> {
    for (const s of this.missions.values()) {
      const link = s.links.find((l) => l.token === token);
      if (!link) continue;
      if (link.revoked) throw this.error(410, 'This link was revoked by the mission owner.');
      if (link.pausedUntil && link.pausedUntil > Date.now()) {
        throw this.error(429, 'Paused after too many wrong PINs', {}, new HttpHeaders({ 'Retry-After': String(Math.ceil((link.pausedUntil - Date.now()) / 1000)) }));
      }
      if (pin !== link.pin) {
        link.wrong++;
        if (link.wrong >= 5) {
          link.wrong = 0;
          link.pausedUntil = Date.now() + 15 * 60e3;
          throw this.error(429, 'Paused after too many wrong PINs', {}, new HttpHeaders({ 'Retry-After': '900' }));
        }
        throw this.error(401, 'Wrong PIN', { triesLeft: 5 - link.wrong });
      }
      link.wrong = 0;
      link.lastOpenedAt = new Date().toISOString();
      const session = `s-${rnd(24)}`;
      this.sessions.set(session, { missionId: s.id, role: link.role, linkId: link.linkId } as never);
      return this.ok({ session, missionId: s.id, role: link.role, missionName: s.mission.name });
    }
    throw this.error(404, 'Unknown link');
  }

  private calcRoute(req: HttpRequest<unknown>): HttpResponse<unknown> {
    const mission = req.body as Mission;
    const result = routeResult(mission, this.scenario());
    if (mission.modules?.['fpv.core']) result.modules = { ...(result.modules ?? {}), 'fpv.core': fpvCoreMock(mission) };
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
    const plan: { name: string; start: number; dur: number; cached?: boolean }[] = [
      { name: 'Elevation for 16 × 16 km', start: 0, dur: 0.1, cached: true },
      { name: `Route profiles, ${job.mission.waypoints.length} legs`, start: 0, dur: 0.9 },
      ...kinds.map((k, i) => ({ name: `Coverage grid, ${k} link`, start: 0.6 + i * 1.3, dur: 1.4 })),
    ];
    const parts = plan.map((p) => {
      if (job.cancelled && t < p.start + p.dur) return { name: p.name, state: 'waiting' as const, progress: 0 };
      if (t >= p.start + p.dur) return { name: p.name, state: p.cached ? ('cached' as const) : ('done' as const), progress: 1 };
      if (t >= p.start) return { name: p.name, state: 'running' as const, progress: (t - p.start) / p.dur };
      return { name: p.name, state: 'waiting' as const, progress: 0 };
    });
    const done = parts.every((p) => p.state === 'done' || p.state === 'cached');
    return { jobId: job.id, state: job.cancelled ? 'cancelled' : done ? 'done' : 'running', parts };
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

  private saveLibrary(kind: LibraryKind, id: string | null, body: unknown): HttpResponse<unknown> {
    if (!LIBRARY_KINDS.some((k) => k.kind === kind)) throw this.error(404, 'Unknown library kind');
    const v = this.validator.validateLibrary(kind, body);
    if (!v.ok) throw this.error(422, 'Schema validation failed', { issues: v.issues });
    const entry = structuredClone(body) as Record<string, unknown>;
    const list = this.library[kind];
    const idx = list.findIndex((e) => e['id'] === (id ?? entry['id']));
    if (id === null && idx >= 0) throw this.error(409, `An entry with id ${entry['id']} exists`);
    if (idx >= 0) {
      entry['version'] = Number(list[idx]['version'] ?? 1) + 1;
      list[idx] = entry;
      return this.ok(entry, 200);
    }
    list.push(entry);
    return this.ok(entry, 201);
  }

  private async uploadModule(req: HttpRequest<unknown>): Promise<HttpResponse<ValidationReport>> {
    const blob = req.body as Blob;
    const name = (blob as File).name ?? 'package.rfmod';
    const size = blob.size ?? 0;
    const head = new Uint8Array(await blob.slice(0, 4).arrayBuffer());
    const isZip = head[0] === 0x50 && head[1] === 0x4b;
    const checks = [
      { name: 'Package format', ok: isZip, detail: isZip ? '.rfmod archive (zip) read' : 'Not a .rfmod archive (expected a zip container)' },
      { name: 'Size', ok: size <= 50 * 1024 * 1024, detail: `${(size / 1024 / 1024).toFixed(2)} MB of max 50 MB` },
      { name: 'Signature', ok: isZip, detail: isZip ? 'Signature valid, publisher key trusted' : 'No signature found' },
      { name: 'Manifest', ok: isZip, detail: isZip ? 'manifest.json matches schema rfmod/v1' : 'manifest.json missing' },
      { name: 'ABI', ok: isZip, detail: 'Needs rf-core ≥1.2 <2, installed 1.3.0; ABI 1' },
      { name: 'Dependencies', ok: isZip, detail: 'Dependencies resolved: rf.weather 1.1.0' },
      { name: 'Deprecated API', ok: true, detail: 'warning: calls link::budget_v1, deprecated in 1.3, removed in 1.6. Use link::budget.' },
      { name: 'Self-test', ok: isZip, detail: isZip ? 'Conformance tests 48 / 48 on reference cases' : 'not run' },
    ];
    void name;
    return this.ok({ ok: checks.every((c) => c.ok), checks });
  }

  private putCache(body: Pick<CacheTier, 'id' | 'limitBytes'>[]): HttpResponse<unknown> {
    for (const b of body ?? []) {
      const t = this.cache.find((c) => c.id === b.id);
      if (t && Number.isFinite(b.limitBytes) && b.limitBytes > 0) t.limitBytes = b.limitBytes;
    }
    return this.ok(this.cache);
  }
}

function publicLink(l: ShareLink & { token?: string; pin?: string }): ShareLink {
  const { linkId, role, label, createdAt, lastOpenedAt, revoked } = l;
  return { linkId, role, label, createdAt, lastOpenedAt, revoked };
}

export const MODULES: ModuleInfo[] = [
  { id: 'rf-core', version: '1.3.0', title: 'Radio core', lifecycle: 'stable', provides: ['link.budget', 'diffraction'], signedBy: 'built in', isolated: false, state: 'active' },
  { id: 'rf.weather', version: '1.1.0', title: 'Weather and refraction', lifecycle: 'stable', requires: ['rf-core>=1.2'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'rf.clutter', version: '0.9.2', title: 'Land cover clutter', lifecycle: 'beta', requires: ['rf-core>=1.2'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'fpv.core', version: '1.0.0', title: 'FPV core', lifecycle: 'beta', provides: ['route.analysis'], signedBy: 'owner', isolated: true, state: 'active' },
  { id: 'fpv.battery', version: '1.0.4', title: 'Battery range', lifecycle: 'stable', requires: ['fpv.core>=1'], signedBy: 'owner', isolated: true, state: 'active' },
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
