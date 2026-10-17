import { Component, OnInit, inject, signal } from '@angular/core';
import { Map as MlMap } from 'maplibre-gl';
import { ApiError, CacheTier, LayerPanelComponent, LayerRegistry, MapViewComponent, RfmapApiClient, createStandardLayers, provideMapView } from '@rfmap/ui-kit';
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
            <div class="tier-head"><b>{{ t.label }}</b><span class="muted small rf-num">{{ gb(t.usedBytes) }} of {{ gb(t.limitBytes) }} GB{{ t.hitRate !== undefined ? ' · hit rate ' + pct(t.hitRate) : '' }}</span></div>
            <div class="bar" role="img" [attr.aria-label]="pct(t.usedBytes / t.limitBytes) + ' used'"><div class="fill" [style.width.%]="Math.min(100, (100 * t.usedBytes) / t.limitBytes)"></div></div>
            <label class="lim">Limit, GB
              <input class="rf-input rf-num" type="number" min="0" step="1" [value]="t.limitBytes / 1e9" (change)="setLimit(t.id, +$any($event.target).value)" />
            </label>
          </div>
        }
        @if (tiers().length) {
          <div class="actions">
            <button type="button" class="rf-btn rf-btn--primary" [disabled]="!dirty() || busy()" (click)="saveCache()">{{ busy() ? 'Saving…' : 'Save limits' }}</button>
            @if (saved()) {<span class="ok-text" role="status">Saved.</span>}
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
  protected readonly saved = signal(false);
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

  protected gb(b: number): string {
    return (b / 1e9).toFixed(1);
  }
  protected pct(x: number): string {
    return `${Math.round(x * 100)} %`;
  }

  protected setLimit(id: string, gb: number): void {
    if (!Number.isFinite(gb) || gb < 0) return;
    this.tiers.update((ts) => ts.map((t) => (t.id === id ? { ...t, limitBytes: Math.round(gb * 1e9) } : t)));
    this.dirty.set(true);
    this.saved.set(false);
  }

  protected async saveCache(): Promise<void> {
    this.busy.set(true);
    try {
      await this.api.putAdminCache(this.tiers().map((t) => ({ id: t.id, limitBytes: t.limitBytes })));
      this.dirty.set(false);
      this.saved.set(true);
    } catch (e) {
      this.cacheError.set(ApiError.from(e).message);
    } finally {
      this.busy.set(false);
    }
  }
}
