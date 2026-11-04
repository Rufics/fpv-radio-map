import { Component, OnInit, computed, inject, input, output, signal } from '@angular/core';
import { Map as MlMap } from 'maplibre-gl';
import {
  ApiError,
  AreaShape,
  AreaShapeLayer,
  LayerRegistry,
  LatLon,
  MapViewComponent,
  OfflineAreaLimits,
  OfflineAreaStatus,
  RfmapApiClient,
  createStandardLayers,
  provideMapView,
} from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';
import { OfflineLibrary } from './offline-library.service';
import { OfflineRegionFormat } from './offline-region-format';

/**
 * Draw a polygon on the map, one corner per click, and ask the server for the offline package
 * of that area. The area and corners are checked against the server's limits while drawing.
 */
@Component({
  selector: 'fpv-area-draw',
  imports: [MapViewComponent],
  providers: [provideMapView()],
  template: `
    <div class="map">
      @if (data.loaded()) {
        <rf-map-view class="mv" label="Map to draw an area on" [config]="data.mapConfig()" [locateButton]="false"
          (mapReady)="onReady($event)" (mapClick)="addCorner($event)" />
      }
    </div>
    <form class="panel" (submit)="$event.preventDefault(); submit()">
      <p class="muted small">Click the map to set the corners of the area; three or more make it.</p>
      <label class="field">Name
        <input class="rf-input" name="name" maxlength="120" [value]="name()" (input)="name.set($any($event.target).value)" />
      </label>
      <dl>
        <dt>Area</dt>
        <dd class="rf-num" [class.over]="overArea()">{{ f.km2(area()) }} of {{ f.km2(limits().maxAreaKm2) }}</dd>
        <dt>Corners</dt>
        <dd class="rf-num" [class.over]="overCorners()">{{ shape().points.length }} of {{ limits().maxVertices }}</dd>
        <dt>Detail</dt>
        <dd>Up to zoom {{ limits().maxzoom }}</dd>
      </dl>
      @if (problem(); as p) {
        <div class="rf-field-error" role="alert">{{ p }}</div>
      }
      <div class="actions">
        <button type="button" class="rf-btn rf-btn--secondary" [disabled]="!shape().points.length || busy()" (click)="undo()">Undo corner</button>
        <button type="button" class="rf-btn rf-btn--secondary" [disabled]="!shape().points.length || busy()" (click)="clear()">Clear</button>
        <button type="submit" class="rf-btn rf-btn--primary" [disabled]="!ready() || busy()">Build package</button>
      </div>
    </form>
  `,
  styles: `
    :host {
      display: grid;
      grid-template-columns: minmax(0, 1fr) 300px;
      gap: 16px;
      align-items: stretch;
    }
    @media (max-width: 760px) {
      :host {
        grid-template-columns: 1fr;
      }
    }
    .map {
      position: relative;
      min-height: 380px;
      border: 1px solid var(--rf-border);
      border-radius: 12px;
      overflow: hidden;
    }
    .mv {
      position: absolute;
      inset: 0;
    }
    .panel {
      display: flex;
      flex-direction: column;
      gap: 10px;
      padding: 14px;
      border: 1px solid var(--rf-border);
      border-radius: 12px;
      background: var(--rf-surface-1);
    }
    .muted {
      color: var(--rf-text-2);
      margin: 0;
    }
    .small {
      font-size: 13px;
    }
    .field {
      display: flex;
      flex-direction: column;
      gap: 4px;
      font-size: 13px;
      color: var(--rf-text-2);
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
    .over {
      color: var(--rf-blocked-text);
    }
    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
    }
    .rf-btn {
      min-height: 44px;
    }
  `,
})
export class AreaDrawComponent implements OnInit {
  protected readonly data = inject(FpvDataService);
  private readonly api = inject(RfmapApiClient);
  private readonly library = inject(OfflineLibrary);
  private readonly layers = inject(LayerRegistry);
  private readonly drawn = new AreaShapeLayer();
  protected readonly f = OfflineRegionFormat;

  readonly limits = input.required<OfflineAreaLimits>();
  /** The server took the request: the area's first status. */
  readonly asked = output<OfflineAreaStatus>();

  readonly shape = signal(new AreaShape());
  readonly name = signal('');
  protected readonly busy = signal(false);
  private readonly refused = signal<string | null>(null);

  protected readonly area = computed(() => this.shape().areaKm2());
  protected readonly overArea = computed(() => this.area() > this.limits().maxAreaKm2);
  protected readonly overCorners = computed(() => this.shape().points.length > this.limits().maxVertices);
  protected readonly crossing = computed(() => this.shape().crossesItself());
  readonly ready = computed(() => this.shape().closed && !this.overArea() && !this.overCorners() && !this.crossing());
  protected readonly problem = computed(() => {
    if (this.overArea()) return `Too large: this server builds areas up to ${this.f.km2(this.limits().maxAreaKm2)}. Make it smaller.`;
    if (this.overCorners()) return `Too many corners: at most ${this.limits().maxVertices}.`;
    if (this.crossing()) return 'Two edges cross. Move the corners so the outline does not cross itself.';
    return this.refused();
  });

  ngOnInit(): void {
    void this.data.load();
  }

  onReady(_map: MlMap): void {
    const cfg = this.data.mapConfig();
    if (cfg) for (const def of createStandardLayers(cfg, new Date().toISOString())) this.layers.register(def);
    this.layers.register(this.drawn);
    this.redraw();
  }

  addCorner(p: LatLon): void {
    if (this.busy()) return;
    this.set(this.shape().with(p));
  }

  protected undo(): void {
    this.set(this.shape().withoutLast());
  }

  protected clear(): void {
    this.set(new AreaShape());
  }

  async submit(): Promise<void> {
    const geometry = this.shape().geometry();
    if (!geometry || !this.ready()) return;
    const name = this.name().trim() || 'Drawn area';
    const request = { name, geometry };
    this.busy.set(true);
    try {
      const status = await this.api.requestOfflineArea(request);
      this.library.remember({ id: status.id, name, request, askedAt: new Date().toISOString() });
      this.asked.emit(status);
      this.name.set('');
      this.set(new AreaShape());
    } catch (e) {
      const err = ApiError.from(e);
      this.refused.set(err.status === 503 ? `${err.message} (in ${err.retryAfterS ?? 60} s)` : err.message);
    } finally {
      this.busy.set(false);
    }
  }

  private set(shape: AreaShape): void {
    this.shape.set(shape);
    this.refused.set(null);
    this.redraw();
  }

  private redraw(): void {
    this.drawn.show(this.shape(), !!this.problem());
  }
}
