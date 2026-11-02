import { Component, OnInit, inject, signal } from '@angular/core';
import { RouterLink } from '@angular/router';
import { ApiError, OfflineRegion, RfmapApiClient } from '@rfmap/ui-kit';
import { OfflineRegionFormat } from './offline-region-format';

/** The regions whose map this server offers for use without a network: what there is to download. */
@Component({
  selector: 'fpv-offline-regions-page',
  imports: [RouterLink],
  template: `
    <header class="bar">
      <a routerLink="/" class="home"><strong>FPV Radio Map</strong></a
      ><span class="sep">/</span>
      <h1>Offline regions</h1>
    </header>
    <main class="content">
      <p class="intro">
        Each package holds the map of one region: base map, relief and fonts, everything the map
        needs to work without a network. A package keeps its version until its map data changes, so
        download it again only when the version here differs from yours.
      </p>
      @if (error(); as e) {
        <div class="rf-field-error" role="alert">{{ e }}</div>
      } @else if (loading()) {
        <p class="muted" role="status">Loading the regions…</p>
      } @else if (!regions().length) {
        <p class="muted">This server offers no offline regions yet.</p>
      }
      <ul class="grid" aria-label="Offline regions">
        @for (r of regions(); track r.id) {
          <li class="card">
            @if (r.preview) {
              <img
                class="preview"
                [src]="r.preview"
                [alt]="'Relief of ' + r.name"
                width="256"
                height="171"
              />
            } @else {
              <div class="preview none" aria-hidden="true">No preview</div>
            }
            <div class="body">
              <h2>{{ r.name }}</h2>
              <span class="muted small rf-num">{{ f.bbox(r) }}</span>
              <dl>
                <dt>Size</dt>
                <dd class="rf-num">{{ f.size(r.size) }}</dd>
                <dt>Layers</dt>
                <dd>{{ f.layers(r) }}</dd>
                <dt>Detail</dt>
                <dd>{{ f.zoom(r) }}</dd>
                <dt>Version</dt>
                <dd>
                  <span class="rf-mono" [attr.title]="r.version">{{ f.version(r) }}</span> ·
                  {{ f.builtOn(r) }}
                </dd>
              </dl>
              <a
                class="rf-btn rf-btn--primary"
                [href]="r.package"
                download
                [attr.aria-label]="'Download ' + r.name + ', ' + f.size(r.size)"
                >Download</a
              >
            </div>
          </li>
        }
      </ul>
    </main>
  `,
  styles: `
    :host {
      display: flex;
      flex-direction: column;
      height: 100%;
    }
    .bar {
      min-height: 56px;
      flex: none;
      display: flex;
      align-items: center;
      gap: 14px;
      padding: 0 20px;
      border-bottom: 1px solid var(--rf-border);
      background: var(--rf-surface-1);
    }
    .home {
      color: var(--rf-text);
      text-decoration: none;
    }
    .sep {
      color: var(--rf-border-strong);
    }
    h1 {
      margin: 0;
      font-size: 16px;
      font-weight: 600;
    }
    .content {
      flex: 1;
      min-height: 0;
      overflow: auto;
      padding: 20px;
      display: flex;
      flex-direction: column;
      gap: 16px;
    }
    .intro {
      margin: 0;
      max-width: 760px;
      color: var(--rf-text-2);
      line-height: 1.5;
    }
    .muted {
      color: var(--rf-text-2);
      margin: 0;
    }
    .small {
      font-size: 12px;
    }
    .grid {
      list-style: none;
      margin: 0;
      padding: 0;
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
      gap: 16px;
    }
    .card {
      display: flex;
      flex-direction: column;
      border: 1px solid var(--rf-border);
      border-radius: 12px;
      overflow: hidden;
      background: var(--rf-surface-1);
    }
    .preview {
      display: block;
      width: 100%;
      height: auto;
      aspect-ratio: 3 / 2;
      object-fit: cover;
      background: var(--rf-surface-3);
    }
    .preview.none {
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--rf-text-3);
      font-size: 13px;
    }
    .body {
      padding: 14px;
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    h2 {
      margin: 0;
      font-size: 16px;
    }
    dl {
      margin: 0;
      display: grid;
      grid-template-columns: auto 1fr;
      gap: 4px 12px;
      font-size: 13px;
    }
    dt {
      color: var(--rf-text-2);
    }
    dd {
      margin: 0;
    }
    .rf-btn {
      align-self: flex-start;
      margin-top: 4px;
      min-height: 44px;
      display: inline-flex;
      align-items: center;
      text-decoration: none;
    }
  `,
})
export class OfflineRegionsPage implements OnInit {
  private readonly api = inject(RfmapApiClient);
  protected readonly f = OfflineRegionFormat;
  protected readonly regions = signal<OfflineRegion[]>([]);
  protected readonly loading = signal(true);
  protected readonly error = signal<string | null>(null);

  ngOnInit(): void {
    void this.load();
  }

  protected async load(): Promise<void> {
    this.loading.set(true);
    this.error.set(null);
    try {
      this.regions.set(await this.api.offlineRegions());
    } catch (e) {
      this.error.set(`Could not load the offline regions: ${ApiError.from(e).message}`);
    } finally {
      this.loading.set(false);
    }
  }
}
