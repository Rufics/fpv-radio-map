import {
  Component,
  DestroyRef,
  ElementRef,
  afterNextRender,
  computed,
  effect,
  inject,
  input,
  output,
  viewChild,
} from '@angular/core';
import { addProtocol, Map as MlMap, NavigationControl } from 'maplibre-gl';
import { Protocol } from 'pmtiles';
import { MapSourcesConfig } from '../models/api.models';
import { LayerRegistry } from '../layers/layer-registry';
import { SlotRegistry } from '../slots/slot-registry';
import { LatLon } from '../tools/geo';
import { MapContext } from './map-context';

let pmtilesRegistered = false;

/** Register the pmtiles:// protocol once per page (map sources come from our server as PMTiles). */
export function ensurePmtilesProtocol(): void {
  if (pmtilesRegistered) return;
  const protocol = new Protocol();
  addProtocol('pmtiles', protocol.tile);
  pmtilesRegistered = true;
}

/**
 * MapLibre wrapper. Needs `provideMapView()` in an ancestor (LayerRegistry + MapContext).
 * Layers come from the LayerRegistry and from `map.layers` slot contributions.
 */
@Component({
  selector: 'rf-map-view',
  template: `
    <div #container class="rf-map-canvas" role="region" [attr.aria-label]="label()"></div>
    <ng-content />
    @if (showAttribution()) {
      <div class="rf-map-attribution">{{ attributionText() }}</div>
    }
  `,
  styles: `
    :host { position: relative; display: block; overflow: hidden; background: var(--rf-map-land, #0E1319); }
    .rf-map-canvas { position: absolute; inset: 0; }
    .rf-map-attribution {
      position: absolute; left: 12px; bottom: 10px; z-index: 2; max-width: calc(100% - 24px);
      font-size: 11px; color: var(--rf-text-2); background: rgba(16, 22, 29, 0.85);
      padding: 4px 8px; border-radius: var(--rf-radius-sm); pointer-events: auto;
    }
  `,
})
export class MapViewComponent {
  readonly config = input<MapSourcesConfig | null>(null);
  readonly label = input('Map');
  readonly showAttribution = input(true);
  readonly interactive = input(true);
  readonly mapClick = output<LatLon>();
  readonly mapReady = output<MlMap>();

  private readonly container = viewChild.required<ElementRef<HTMLDivElement>>('container');
  private readonly layers = inject(LayerRegistry);
  private readonly ctx = inject(MapContext);
  private readonly slots = inject(SlotRegistry);
  private readonly destroyRef = inject(DestroyRef);
  private map?: MlMap;
  private readonly slotLayerCleanups = new Map<string, () => void>();

  protected readonly attributionText = computed(() => {
    const parts = [...(this.config()?.attribution ?? ['© OpenStreetMap contributors', 'Copernicus DEM'])];
    for (const a of this.layers.attributions()) {
      if (parts.some((p) => p.includes(a))) continue;
      // a longer credit replaces a shorter one it contains ("© OpenMapTiles © OpenStreetMap contributors")
      for (let i = parts.length - 1; i >= 0; i--) if (a.includes(parts[i])) parts.splice(i, 1);
      parts.push(a);
    }
    return parts.join(' · ');
  });

  constructor() {
    afterNextRender(() => this.create());
    // Bridge `map.layers` slot contributions into the layer registry.
    effect(() => {
      const wanted = this.slots.slot('map.layers')();
      const ids = new Set(wanted.map((c) => c.id));
      for (const [id, off] of this.slotLayerCleanups) {
        if (!ids.has(id)) {
          off();
          this.slotLayerCleanups.delete(id);
        }
      }
      for (const c of wanted) {
        if (c.layer && !this.slotLayerCleanups.has(c.id)) {
          this.slotLayerCleanups.set(c.id, this.layers.register(c.layer));
        }
      }
    });
    this.destroyRef.onDestroy(() => {
      this.layers.disconnect();
      this.ctx.map.set(null);
      this.map?.remove();
    });
  }

  private create(): void {
    ensurePmtilesProtocol();
    const cfg = this.config();
    const map = new MlMap({
      container: this.container().nativeElement,
      style: { version: 8, sources: {}, layers: [], ...(cfg?.glyphs ? { glyphs: cfg.glyphs } : {}) },
      center: cfg?.center ?? [35.2, 48.53],
      zoom: cfg?.zoom ?? 11.5,
      attributionControl: false,
      interactive: this.interactive(),
      dragRotate: false,
      pitchWithRotate: false,
      fadeDuration: 0,
    });
    map.touchZoomRotate.disableRotation();
    if (this.interactive()) map.addControl(new NavigationControl({ showCompass: false }), 'bottom-right');
    map.on('click', (e) => {
      const p = { lat: e.lngLat.lat, lon: e.lngLat.lng };
      this.ctx.emitClick(p);
      this.mapClick.emit(p);
    });
    map.on('load', () => {
      this.layers.connect(map);
      this.ctx.map.set(map);
      this.mapReady.emit(map);
    });
    this.map = map;
    new ResizeObserver(() => map.resize()).observe(this.container().nativeElement);
  }
}
