import { Component, OnInit, inject, signal } from '@angular/core';
import { Map as MlMap } from 'maplibre-gl';
import { ApiError, CacheTier, CacheTierId, LayerPanelComponent, LayerRegistry, MapViewComponent, RfmapApiClient, createStandardLayers, provideMapView } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';

/** Map layers and data sources (MapSources.dc.html) plus the server cache limits. */
@Component({
  selector: 'fpv-sources-page',
  imports: [MapViewComponent, LayerPanelComponent],
  providers: [provideMapView()],
  template: `
    <div class="layout">
      <div class="map">
        @if (data.loaded()) {
          <rf-map-view class="mv" label="Data sources preview map" [config]="data.mapConfig()" (mapReady)="onReady($event)">
            <rf-layer-panel class="panel" [forecastOffset]="offset()" (forecastOffsetChange)="offset.set($event)" />
          </rf-map-view>
        }
      </div>
      <section class="side">
        <h1>Data sources <span class="rf-badge rf-badge--admin">Admin</span></h1>
        <p class="muted">All free. Everything except the weather forecast is stored on our server, so the app works without third-party map services.</p>
        <div class="table" role="table" aria-label="Data sources">
          <div class="tr th" role="row"><span role="columnheader">Layer</span><span role="columnheader">Source</span><span role="columnheader">Detail</span><span role="columnheader">Licence</span></div>
          @for (r of rows; track r[0]) {
            <div class="tr" role="row">@for (c of r; track $index) {<span role="cell">{{ c }}</span>}</div>
          }
        </div>
        <div class="note">One DEM for maps and maths. Hillshade and contours come from the exact elevation file the calculation uses, so what you see and what is computed always agree.</div>

        <h2>Cache limits</h2>
        @if (cacheError(); as e) {<div class="rf-field-error" role="alert">{{ e }}</div>}
        @for (t of tiers(); track t.id) {
          <div class="tier">
            <div class="tier-head">
              <b>{{ t.label ?? t.id }}</b>
              <span class="muted small rf-num">{{ usage(t) }}</span>
            </div>
            @if (fraction(t); as f) {
              <div class="bar" role="img" [attr.aria-label]="pct(f) + ' used'"><div class="fill" [style.width.%]="Math.min(100, 100 * f)"></div></div>
            }
            @if (t.entries !== undefined || t.hits !== undefined) {
              <span class="muted small rf-num">{{ t.entries ?? '–' }} entries · {{ t.hits ?? '–' }} hits · {{ t.misses ?? '–' }} misses · {{ t.evictions ?? '–' }} evictions</span>
            }
            <div class="lims">
              <label class="lim">Limit, GB
                <input class="rf-input rf-num" type="number" min="0" step="0.1" [value]="t.limitBytes !== undefined ? t.limitBytes / 1e9 : ''" (change)="setLimit(t.id, 'limitBytes', +$any($event.target).value * 1e9)" />
              </label>
              @if (t.id === 'results-table') {
                <label class="lim">Limit, rows
                  <input class="rf-input rf-num" type="number" min="0" step="1000" [value]="t.limitRows ?? ''" (change)="setLimit(t.id, 'limitRows', +$any($event.target).value)" />
                </label>
              }
              <button type="button" class="rf-btn rf-btn--secondary" [disabled]="busy()" (click)="clear(t.id)">Clear</button>
            </div>
          </div>
        }
        @if (tiers().length) {
          <div class="actions">
            <button type="button" class="rf-btn rf-btn--primary" [disabled]="!dirty() || busy()" (click)="saveCache()">{{ busy() ? 'Saving…' : 'Save limits' }}</button>
            <button type="button" class="rf-btn rf-btn--secondary" [disabled]="busy()" (click)="clear('all')">Clear all caches</button>
            @if (saved(); as msg) {<span class="ok-text" role="status">{{ msg }}</span>}
          </div>
        }
      </section>
    </div>
  `,
  styles: `
    .layout { display: grid; grid-template-columns: minmax(0, 1fr) 560px; height: 100%; }
    .map { position: relative; min-height: 420px; }
    .mv { position: absolute; inset: 0; }
    .panel { position: absolute; top: 12px; left: 12px; z-index: 3; width: 260px; }
    .side { border-left: 1px solid var(--rf-border); background: var(--rf-surface-2); padding: 18px; overflow-y: auto; display: flex; flex-direction: column; gap: 12px; }
    .side > * { flex-shrink: 0; }
    h1 { margin: 0; font-size: 20px; display: flex; gap: 8px; align-items: center; }
    h2 { margin: 8px 0 0; font-size: 16px; }
    .muted { color: var(--rf-text-2); }
    .small { font-size: 12px; }
    p { margin: 0; }
    .table { border: 1px solid var(--rf-border); border-radius: 12px; overflow: hidden; font-size: 13px; }
    .tr { display: grid; grid-template-columns: 1.1fr 1.8fr 0.7fr 1.2fr; border-top: 1px solid var(--rf-divider); }
    .tr > span { padding: 8px 10px; }
    .tr.th { background: var(--rf-surface-3); color: var(--rf-text-2); font-weight: 600; font-size: 12px; border-top: 0; }
    .note { padding: 12px; border-radius: 12px; background: var(--rf-surface-1); border: 1px solid var(--rf-border); color: var(--rf-text-3); font-size: 13px; line-height: 1.45; }
    .tier { display: flex; flex-direction: column; gap: 6px; padding: 10px 12px; border: 1px solid var(--rf-border); border-radius: 12px; background: var(--rf-surface-1); }
    .tier-head { display: flex; justify-content: space-between; gap: 8px; flex-wrap: wrap; }
    .bar { height: 6px; border-radius: 3px; background: var(--rf-surface-3); overflow: hidden; }
    .fill { height: 100%; background: var(--rf-ok); }
    .lims { display: flex; flex-wrap: wrap; gap: 12px; align-items: center; }
    .lim { display: flex; align-items: center; gap: 8px; font-size: 13px; color: var(--rf-text-2); }
    .lim input { width: 100px; height: 32px; }
    .actions { display: flex; align-items: center; gap: 10px; }
    .ok-text { color: var(--rf-ok-text); font-size: 13px; }
    @media (max-width: 1100px) { .layout { grid-template-columns: 1fr; height: auto; } .map { height: 420px; } .side { border-left: 0; } }
  `,
})
export class SourcesPage implements OnInit {
  protected readonly data = inject(FpvDataService);
  private readonly api = inject(RfmapApiClient);
  private readonly layers = inject(LayerRegistry);
  protected readonly Math = Math;
  protected readonly offset = signal(0);
  protected readonly tiers = signal<CacheTier[]>([]);
  protected readonly dirty = signal(false);
  protected readonly busy = signal(false);
  protected readonly saved = signal<string | null>(null);
  protected readonly cacheError = signal<string | null>(null);

  protected readonly rows: string[][] = [
    ['Base map', 'OpenStreetMap → PMTiles (our builder)', 'z0–16', 'ODbL, credit'],
    ['Elevation for maths', 'Copernicus DEM GLO-30; SRTM as fallback', '30 m', 'Free, credit'],
    ['Hillshade, contours', 'Made locally from the same DEM', '30 m', 'as DEM'],
    ['Satellite', 'Sentinel-2 mosaic → raster PMTiles', '10 m', 'Free; terms differ by mosaic, checked before release'],
    ['Land cover', 'ESA WorldCover', '10 m', 'CC BY 4.0'],
    ['Weather forecast', 'Open-Meteo, self-hosted; ICON, GFS, ECMWF open data', 'hourly', 'Open data, credit'],
    ['Weather map', 'Tiles drawn from the same forecast', 'hourly', 'as forecast'],
  ];

  async ngOnInit(): Promise<void> {
    await this.data.load();
    try {
      this.tiers.set(await this.api.adminCache());
    } catch (e) {
      const err = ApiError.from(e);
      this.cacheError.set(err.status === 401 || err.status === 403 ? 'Set the admin token (top right) to see and change cache limits.' : err.message);
    }
  }

  onReady(_map: MlMap): void {
    const cfg = this.data.mapConfig();
    if (!cfg) return;
    for (const def of createStandardLayers(cfg, new Date().toISOString())) this.layers.register(def);
  }

  protected usage(t: CacheTier): string {
    const gb = (b: number | undefined) => (b === undefined ? '–' : (b / 1e9).toFixed(2));
    const parts = [`${gb(t.usedBytes)} of ${gb(t.limitBytes)} GB`];
    if (t.limitRows !== undefined) parts.push(`max ${t.limitRows.toLocaleString('en-US')} rows`);
    if (t.hitRate !== undefined) parts.push(`hit rate ${this.pct(t.hitRate)}`);
    return parts.join(' · ');
  }
  /** Used share, or null when the server did not send both numbers (no bar then). */
  protected fraction(t: CacheTier): number | null {
    return t.usedBytes !== undefined && t.limitBytes ? Math.max(t.usedBytes / t.limitBytes, 0.0001) : null;
  }
  protected pct(x: number): string {
    return `${Math.round(x * 100)} %`;
  }

  protected setLimit(id: CacheTierId, field: 'limitBytes' | 'limitRows', value: number): void {
    if (!Number.isFinite(value) || value < 0) return;
    this.tiers.update((ts) => ts.map((t) => (t.id === id ? { ...t, [field]: Math.round(value) } : t)));
    this.dirty.set(true);
    this.saved.set(null);
  }

  protected async saveCache(): Promise<void> {
    this.busy.set(true);
    this.cacheError.set(null);
    try {
      const body = this.tiers().map((t) => ({ id: t.id, limitBytes: t.limitBytes, ...(t.id === 'results-table' && t.limitRows !== undefined ? { limitRows: t.limitRows } : {}) }));
      this.tiers.set(await this.api.putAdminCache(body));
      this.dirty.set(false);
      this.saved.set('Saved. Limits survive a restart.');
    } catch (e) {
      this.cacheError.set(ApiError.from(e).message);
    } finally {
      this.busy.set(false);
    }
  }

  protected async clear(tier: CacheTierId | 'all'): Promise<void> {
    this.busy.set(true);
    this.cacheError.set(null);
    try {
      await this.api.clearCache(tier);
      this.tiers.set(await this.api.adminCache());
      this.saved.set(tier === 'all' ? 'All caches cleared.' : `${tier} cleared.`);
    } catch (e) {
      this.cacheError.set(ApiError.from(e).message);
    } finally {
      this.busy.set(false);
    }
  }
}
