import { HttpClient, HttpErrorResponse, HttpHeaders, HttpParams } from '@angular/common/http';
import { Injectable, InjectionToken, inject } from '@angular/core';
import { Observable, firstValueFrom } from 'rxjs';
import { map } from 'rxjs/operators';
import {
  CacheLimit,
  CacheTier,
  CalcPathRequest,
  CoverageGrid,
  CreateLinkRequest,
  CreateMissionResponse,
  CreatedShareLink,
  ForecastHorizon,
  GetMissionResponse,
  LibraryHistoryEntry,
  LinkInfo,
  MissingData,
  Job,
  LinkResult,
  MapSourcesConfig,
  ModuleInfo,
  OpenLinkResponse,
  Profile,
  RouteResult,
  ShareLink,
  TrustedKey,
  ValidationReport,
  WeatherPoint,
} from '../models/api.models';
import { LibraryKind } from '../models/library.models';
import { LinkKind, Mission } from '../models/mission.models';
import { CredentialStore } from './credential-store';

/** Base URL of the rfmap API, `/api/v1` per the contract's `servers`. */
export const RFMAP_API_BASE = new InjectionToken<string>('RFMAP_API_BASE', { factory: () => '/api/v1' });

/** Typed error for the cases the UI treats specially. */
export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
    readonly body?: unknown,
    readonly retryAfterS?: number,
  ) {
    super(message);
  }

  static from(e: unknown): ApiError {
    if (e instanceof ApiError) return e;
    if (e instanceof HttpErrorResponse) {
      const body = e.error;
      // 429 and 503 carry retryAfterS in the body and Retry-After in the header
      const fromBody = body && typeof body === 'object' ? Number((body as { retryAfterS?: unknown }).retryAfterS) : Number.NaN;
      const retry = Number.isFinite(fromBody) && fromBody > 0 ? fromBody : Number(e.headers?.get('Retry-After'));
      const msg = (body && typeof body === 'object' && 'message' in body && String((body as { message: unknown }).message)) || e.message;
      return new ApiError(e.status, msg, body, Number.isFinite(retry) && retry > 0 ? retry : undefined);
    }
    return new ApiError(0, e instanceof Error ? e.message : String(e));
  }

  /** The 422 "missing-data" body of /calc/path and /elevation/profile, if this is one. */
  get missingData(): MissingData | null {
    const b = this.body as MissingData | undefined;
    return this.status === 422 && b && (b.status === 'missing-data' || !!b.missingData) ? b : null;
  }

  /** The 422 schema report: JSON pointers into the request body. */
  get issues(): { path: string; message: string }[] {
    const b = this.body as { issues?: { path?: string; message?: string }[] } | undefined;
    return this.status === 422 && Array.isArray(b?.issues) ? b.issues.map((i) => ({ path: i.path ?? '', message: i.message ?? '' })) : [];
  }
}

/** One method per operation in docs/api/openapi.yaml (+ the gaps listed in API-NOTES.md). */
@Injectable({ providedIn: 'root' })
export class RfmapApiClient {
  private readonly http = inject(HttpClient);
  private readonly base = inject(RFMAP_API_BASE);
  private readonly creds = inject(CredentialStore);

  private url(path: string): string {
    return `${this.base}${path}`;
  }

  private sessionHeaders(missionId: string | null, session?: string | null): HttpHeaders {
    const s = session ?? this.creds.session(missionId);
    return s ? new HttpHeaders({ Authorization: `Bearer ${s}` }) : new HttpHeaders();
  }

  private adminHeaders(): HttpHeaders {
    const t = this.creds.adminToken();
    return t ? new HttpHeaders({ Authorization: `Bearer ${t}` }) : new HttpHeaders();
  }

  private ownerHeaders(missionId: string, ownerKey?: string): HttpHeaders {
    const key = ownerKey ?? this.creds.ownerKey(missionId);
    return key ? new HttpHeaders({ 'X-Owner-Key': key }) : new HttpHeaders();
  }

  private async run<T>(obs: Observable<T>): Promise<T> {
    try {
      return await firstValueFrom(obs);
    } catch (e) {
      throw ApiError.from(e);
    }
  }

  // ---- missions ----
  createMission(mission: Mission): Promise<CreateMissionResponse> {
    return this.run(this.http.post<CreateMissionResponse>(this.url('/missions'), mission));
  }

  getMission(missionId: string): Promise<GetMissionResponse> {
    return this.run(this.http.get<GetMissionResponse>(this.url(`/missions/${encodeURIComponent(missionId)}`), { headers: this.sessionHeaders(missionId) }));
  }

  /** PUT with If-Match; resolves with the new revision from the ETag. 409 → ApiError(409). */
  saveMission(missionId: string, mission: Mission, revision: number): Promise<number> {
    const headers = this.sessionHeaders(missionId).set('If-Match', `"${revision}"`);
    return this.run(
      this.http.put(this.url(`/missions/${encodeURIComponent(missionId)}`), mission, { headers, observe: 'response' }).pipe(
        map((res) => {
          const etag = res.headers.get('ETag')?.replace(/[W/"]/g, '');
          const n = Number(etag);
          return Number.isFinite(n) && etag ? n : revision + 1;
        }),
      ),
    );
  }

  // ---- links ----
  listLinks(missionId: string, ownerKey?: string): Promise<ShareLink[]> {
    return this.run(this.http.get<ShareLink[]>(this.url(`/missions/${encodeURIComponent(missionId)}/links`), { headers: this.ownerHeaders(missionId, ownerKey) }));
  }

  createLink(missionId: string, req: CreateLinkRequest, ownerKey?: string): Promise<CreatedShareLink> {
    if (!/^\d{6}$/.test(req.pin)) return Promise.reject(new ApiError(400, 'The PIN must be 6 digits'));
    return this.run(this.http.post<CreatedShareLink>(this.url(`/missions/${encodeURIComponent(missionId)}/links`), req, { headers: this.ownerHeaders(missionId, ownerKey) }));
  }

  revokeLink(missionId: string, linkId: string, ownerKey?: string): Promise<void> {
    return this.run(
      this.http
        .delete(this.url(`/missions/${encodeURIComponent(missionId)}/links/${encodeURIComponent(linkId)}`), { headers: this.ownerHeaders(missionId, ownerKey) })
        .pipe(map(() => undefined)),
    );
  }

  /** Public, before the PIN: role, mission name, expiry, pause. 404 unknown, 410 revoked or expired. */
  linkInfo(token: string): Promise<LinkInfo> {
    return this.run(this.http.get<LinkInfo>(this.url(`/links/${encodeURIComponent(token)}`)));
  }

  /** 401 wrong PIN (body: triesLeft), 410 revoked or expired, 429 paused (retryAfterS). */
  openLink(token: string, pin: string): Promise<OpenLinkResponse> {
    return this.run(this.http.post<OpenLinkResponse>(this.url(`/links/${encodeURIComponent(token)}/open`), { pin }));
  }

  // ---- calc ----
  calcPath(missionId: string | null, req: CalcPathRequest): Promise<LinkResult> {
    return this.run(this.http.post<LinkResult>(this.url('/calc/path'), req, { headers: this.sessionHeaders(missionId) }));
  }

  calcRoute(missionId: string | null, mission: Mission): Observable<RouteResult> {
    return this.http.post<RouteResult>(this.url('/calc/route'), mission, { headers: this.sessionHeaders(missionId) });
  }

  startCoverage(missionId: string | null, mission: Mission): Promise<Job> {
    return this.run(this.http.post<Job>(this.url('/calc/coverage'), mission, { headers: this.sessionHeaders(missionId) }));
  }

  getJob(missionId: string | null, jobId: string): Promise<Job> {
    return this.run(this.http.get<Job>(this.url(`/jobs/${encodeURIComponent(jobId)}`), { headers: this.sessionHeaders(missionId) }));
  }

  cancelJob(missionId: string | null, jobId: string): Promise<void> {
    return this.run(this.http.delete(this.url(`/jobs/${encodeURIComponent(jobId)}`), { headers: this.sessionHeaders(missionId) }).pipe(map(() => undefined)));
  }

  getCoverage(missionId: string | null, jobId: string, link: LinkKind): Promise<CoverageGrid> {
    return this.run(this.http.get<CoverageGrid>(this.url(`/jobs/${encodeURIComponent(jobId)}/coverage/${link}.json`), { headers: this.sessionHeaders(missionId) }));
  }

  elevationProfile(from: { lat: number; lon: number }, to: { lat: number; lon: number }): Promise<Profile> {
    const params = new HttpParams().set('from', `${from.lat},${from.lon}`).set('to', `${to.lat},${to.lon}`);
    return this.run(this.http.get<Profile>(this.url('/elevation/profile'), { params }));
  }

  // ---- library ----
  library<T = Record<string, unknown>>(kind: LibraryKind): Promise<T[]> {
    return this.run(this.http.get<T[]>(this.url(`/library/${kind}`)));
  }

  addLibraryEntry(kind: LibraryKind, entry: unknown): Promise<unknown> {
    return this.run(this.http.post(this.url(`/library/${kind}`), entry, { headers: this.adminHeaders() }));
  }

  /** Save as a new version; `entry.version` must be the version you edited (409 if it changed). */
  saveLibraryEntry(kind: LibraryKind, id: string, entry: unknown): Promise<unknown> {
    return this.run(this.http.put(this.url(`/library/${kind}/${encodeURIComponent(id)}`), entry, { headers: this.adminHeaders() }));
  }

  libraryItem<T = Record<string, unknown>>(kind: LibraryKind, id: string): Promise<T> {
    return this.run(this.http.get<T>(this.url(`/library/${kind}/${encodeURIComponent(id)}`)));
  }

  libraryHistory(kind: LibraryKind, id: string): Promise<LibraryHistoryEntry[]> {
    return this.run(this.http.get<LibraryHistoryEntry[]>(this.url(`/library/${kind}/${encodeURIComponent(id)}/history`)));
  }

  // ---- weather ----
  weatherPoint(lat: number, lon: number, timeUtc?: string): Promise<WeatherPoint> {
    let params = new HttpParams().set('lat', lat).set('lon', lon);
    if (timeUtc) params = params.set('timeUtc', timeUtc);
    return this.run(this.http.get<WeatherPoint>(this.url('/weather/point'), { params }));
  }

  weatherHorizon(): Promise<ForecastHorizon> {
    return this.run(this.http.get<ForecastHorizon>(this.url('/weather/horizon')));
  }

  // ---- modules / admin ----
  modules(): Promise<ModuleInfo[]> {
    return this.run(this.http.get<ModuleInfo[]>(this.url('/modules')));
  }

  /** `install=false` only checks the package. */
  uploadModule(pkg: Blob, install: boolean): Promise<ValidationReport> {
    const params = new HttpParams().set('install', String(install));
    const headers = this.adminHeaders().set('Content-Type', 'application/octet-stream');
    return this.run(this.http.post<ValidationReport>(this.url('/admin/modules'), pkg, { headers, params }));
  }

  /** Enable or disable a module (built-ins included) and set its server-wide options. */
  setModule(moduleId: string, body: { enabled?: boolean; options?: Record<string, unknown> }): Promise<ModuleInfo[]> {
    return this.run(this.http.put<ModuleInfo[]>(this.url(`/admin/modules/${encodeURIComponent(moduleId)}`), body, { headers: this.adminHeaders() }));
  }

  removeModule(moduleId: string): Promise<void> {
    return this.run(this.http.delete(this.url(`/admin/modules/${encodeURIComponent(moduleId)}`), { headers: this.adminHeaders() }).pipe(map(() => undefined)));
  }

  moduleNotices(): Promise<Record<string, unknown>[]> {
    return this.run(this.http.get<Record<string, unknown>[]>(this.url('/admin/modules/notices'), { headers: this.adminHeaders() }));
  }

  adminKeys(): Promise<TrustedKey[]> {
    return this.run(this.http.get<TrustedKey[]>(this.url('/admin/keys'), { headers: this.adminHeaders() }));
  }

  /** Add a trusted signing key; returns the key list. */
  addKey(key: string, label?: string): Promise<TrustedKey[]> {
    return this.run(this.http.post<TrustedKey[]>(this.url('/admin/keys'), label ? { key, label } : { key }, { headers: this.adminHeaders() }));
  }

  /** 409 for the built-in owner key or an unknown key. */
  deleteKey(keyId: string): Promise<void> {
    return this.run(this.http.delete(this.url(`/admin/keys/${encodeURIComponent(keyId)}`), { headers: this.adminHeaders() }).pipe(map(() => undefined)));
  }

  adminCache(): Promise<CacheTier[]> {
    return this.run(this.http.get<CacheTier[]>(this.url('/admin/cache'), { headers: this.adminHeaders() }));
  }

  /** Change limits; returns the tiers. */
  putAdminCache(limits: CacheLimit[]): Promise<CacheTier[]> {
    return this.run(this.http.put<CacheTier[]>(this.url('/admin/cache'), limits, { headers: this.adminHeaders() }));
  }

  clearCache(tier: 'all' | CacheTier['id'] = 'all'): Promise<void> {
    const params = new HttpParams().set('tier', tier);
    return this.run(this.http.delete(this.url('/admin/cache'), { headers: this.adminHeaders(), params }).pipe(map(() => undefined)));
  }

  /** GET /config/map. Falls back to the app environment when the server cannot be reached. */
  mapConfig(): Promise<MapSourcesConfig> {
    return this.run(this.http.get<MapSourcesConfig>(this.url('/config/map')));
  }
}
