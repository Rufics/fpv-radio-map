import { Component, DestroyRef, OnInit, computed, inject, signal } from '@angular/core';
import { RouterLink } from '@angular/router';
import { ApiError, OfflineAreaLimits, OfflineAreaStatus, OfflineRegion, RfmapApiClient } from '@rfmap/ui-kit';
import { AreaDrawComponent } from './area-draw.component';
import { DrawnArea, OfflineLibrary } from './offline-library.service';
import { OfflinePackageCardComponent } from './offline-package-card.component';
import { OfflineRegionFormat } from './offline-region-format';

/** Where the build of a drawn area stands, as this page last heard. */
type AreaView = OfflineAreaStatus | { id: string; state: 'gone' } | { id: string; state: 'error'; error: string };

/**
 * The map of a region for use without a network: the regions this server offers, areas the
 * user draws, and the packages saved on this device, with the one the map reads.
 */
@Component({
  selector: 'fpv-offline-regions-page',
  imports: [RouterLink, OfflinePackageCardComponent, AreaDrawComponent],
  template: `
    <header class="bar">
      <a routerLink="/" class="home"><strong>FPV Radio Map</strong></a
      ><span class="sep">/</span>
      <h1>Offline regions</h1>
    </header>
    <main class="content">
      <p class="intro">
        Each package holds the map of one region: base map, relief and fonts, everything the map
        needs to work without a network. Save one on this device and the map reads it instead of
        the server. A package keeps its version until its map data changes; when the server has a
        newer one, its card says so.
      </p>

      <section aria-labelledby="device-h" class="device">
        <h2 id="device-h">On this device</h2>
        @if (!library.persistent) {
          <p class="rf-field-error" role="alert">
            This browser keeps no data for the app, so saved packages last only while this page is open.
          </p>
        }
        @if (library.saved().length) {
          <p class="muted small rf-num">
            {{ library.saved().length }} saved, {{ f.size(library.savedBytes()) }} together. The map reads the one chosen here
            (open the map again after a change).
          </p>
          <ul class="saved" aria-label="Saved packages">
            <li>
              <label><input type="radio" name="active" [checked]="!library.active()" (change)="library.setActive(null)" />
                The server's map</label>
            </li>
            @for (s of library.saved(); track s.key) {
              <li>
                <label><input type="radio" name="active" [checked]="library.active() === s.key" (change)="library.setActive(s.key)" />
                  {{ s.region.name }}</label>
                <span class="muted small rf-num">{{ f.size(s.region.size) }} · <span class="rf-mono">{{ f.version(s.region) }}</span></span>
                <button type="button" class="rf-btn rf-btn--secondary" [attr.aria-label]="'Remove ' + s.region.name + ' from this device'"
                  (click)="library.remove(s.key)">Remove</button>
              </li>
            }
          </ul>
        } @else {
          <p class="muted small">Nothing saved yet. Save a region or a drawn area below.</p>
        }
      </section>

      <section aria-labelledby="regions-h">
        <h2 id="regions-h">Regions of this server</h2>
        @if (error(); as e) {
          <div class="rf-field-error" role="alert">{{ e }}</div>
        } @else if (loading()) {
          <p class="muted" role="status">Loading the regions…</p>
        } @else if (!regions().length) {
          <p class="muted">This server offers no offline regions yet.</p>
        }
        <ul class="grid" aria-label="Offline regions">
          @for (r of regions(); track r.id) {
            <li class="card"><fpv-offline-package-card [region]="r" /></li>
          }
        </ul>
      </section>

      @if (limits()?.enabled) {
        <section aria-labelledby="draw-h">
          <h2 id="draw-h">Draw an area</h2>
          <p class="muted small">
            Not in the list? Draw the area you need and the server builds its package, usually within minutes.
          </p>
          <fpv-area-draw [limits]="limits()!" (asked)="follow($event)" />
        </section>
      }

      @if (library.drawn().length) {
        <section aria-labelledby="drawn-h">
          <h2 id="drawn-h">Your drawn areas</h2>
          <ul class="grid" aria-label="Drawn areas">
            @for (a of library.drawn(); track a.id) {
              @let v = areas()[a.id];
              <li class="card area">
                @if (v?.state === 'ready' && packageOf(v); as p) {
                  <fpv-offline-package-card [region]="p" kind="areas" [request]="a.request" />
                } @else {
                  <div class="pending">
                    <h3>{{ a.name }}</h3>
                    <p class="small" role="status">{{ stateText(v) }}</p>
                  </div>
                }
                <div class="area-actions">
                  <button type="button" class="rf-btn rf-btn--secondary" [disabled]="busy()" (click)="ask(a)">
                    {{ v?.state === 'gone' || v?.state === 'failed' ? 'Ask again' : 'Check for an update' }}
                  </button>
                  <button type="button" class="rf-btn rf-btn--secondary" [attr.aria-label]="'Forget ' + a.name" (click)="library.forget(a.id)">
                    Forget
                  </button>
                </div>
              </li>
            }
          </ul>
        </section>
      }
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
    h2 {
      margin: 0 0 10px;
      font-size: 15px;
    }
    h3 {
      margin: 0;
      font-size: 15px;
    }
    .content {
      flex: 1;
      min-height: 0;
      overflow: auto;
      padding: 20px;
      display: flex;
      flex-direction: column;
      gap: 24px;
    }
    .intro {
      margin: 0;
      max-width: 760px;
      color: var(--rf-text-2);
      line-height: 1.5;
    }
    .muted {
      color: var(--rf-text-2);
      margin: 0 0 8px;
    }
    .small {
      font-size: 13px;
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
    }
    .card > fpv-offline-package-card {
      flex: 1;
    }
    .area {
      gap: 8px;
    }
    .pending {
      padding: 14px;
      border: 1px solid var(--rf-border);
      border-radius: 12px;
      background: var(--rf-surface-1);
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .pending p {
      margin: 0;
      color: var(--rf-text-2);
    }
    .area-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
    }
    .saved {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 6px;
      max-width: 760px;
    }
    .saved li {
      display: flex;
      align-items: center;
      gap: 12px;
      flex-wrap: wrap;
      min-height: 44px;
    }
    .saved label {
      display: flex;
      align-items: center;
      gap: 8px;
      flex: 1;
      min-width: 200px;
    }
    .rf-btn {
      min-height: 44px;
    }
  `,
})
export class OfflineRegionsPage implements OnInit {
  private readonly api = inject(RfmapApiClient);
  protected readonly library = inject(OfflineLibrary);
  protected readonly f = OfflineRegionFormat;
  protected readonly regions = signal<OfflineRegion[]>([]);
  protected readonly limits = signal<OfflineAreaLimits | null>(null);
  protected readonly areas = signal<Record<string, AreaView>>({});
  protected readonly loading = signal(true);
  protected readonly busy = signal(false);
  protected readonly error = signal<string | null>(null);
  private readonly building = computed(() =>
    Object.values(this.areas()).some((a) => a.state === 'queued' || a.state === 'building'),
  );
  private timer?: ReturnType<typeof setTimeout>;
  static readonly pollMs = 3000;

  constructor() {
    inject(DestroyRef).onDestroy(() => clearTimeout(this.timer));
  }

  ngOnInit(): void {
    void this.load();
    void this.library.refresh();
    this.api.offlineAreaLimits().then(
      (l) => this.limits.set(l),
      () => this.limits.set(null),
    );
    void this.poll();
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

  /** The area the server just took: shown with the others and followed until it is built. */
  follow(status: OfflineAreaStatus): void {
    this.areas.update((a) => ({ ...a, [status.id]: status }));
    this.schedule();
  }

  /** Asks for the area again: builds it anew after a failure or removal, or a newer one after the world data changed. */
  protected async ask(a: DrawnArea): Promise<void> {
    this.busy.set(true);
    try {
      this.follow(await this.api.requestOfflineArea(a.request));
    } catch (e) {
      this.areas.update((v) => ({ ...v, [a.id]: { id: a.id, state: 'error', error: ApiError.from(e).message } }));
    } finally {
      this.busy.set(false);
    }
  }

  protected packageOf(v: AreaView | undefined): OfflineRegion | null {
    return v && 'package' in v ? v.package : null;
  }

  protected stateText(v: AreaView | undefined): string {
    switch (v?.state) {
      case undefined:
        return 'Asking the server…';
      case 'queued':
        return v.position === 1 ? 'Waiting: next to be built.' : `Waiting: ${v.position} in the queue.`;
      case 'building':
        return 'Building the package…';
      case 'failed':
        return `Not built: ${v.error ?? 'the server gave no reason'}.`;
      case 'gone':
        return 'The server no longer has this area (unused for a while). Ask again to build it.';
      case 'error':
        return v.error;
      default:
        return '';
    }
  }

  /** Reads the status of every drawn area, then again while any is still being built. */
  private async poll(): Promise<void> {
    const results = await Promise.all(
      this.library.drawn().map(async (a): Promise<AreaView> => {
        try {
          return await this.api.offlineArea(a.id);
        } catch (e) {
          const err = ApiError.from(e);
          return err.status === 404 ? { id: a.id, state: 'gone' } : { id: a.id, state: 'error', error: err.message };
        }
      }),
    );
    this.areas.update((v) => ({ ...v, ...Object.fromEntries(results.map((r) => [r.id, r])) }));
    this.schedule();
  }

  private schedule(): void {
    clearTimeout(this.timer);
    if (this.building()) this.timer = setTimeout(() => void this.poll(), OfflineRegionsPage.pollMs);
  }
}
