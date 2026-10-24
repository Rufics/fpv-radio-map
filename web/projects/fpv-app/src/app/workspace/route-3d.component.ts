import { Component, DestroyRef, computed, effect, inject, signal, untracked } from '@angular/core';
import { Map as MlMap } from 'maplibre-gl';
import { MapContext, TileSourceRef } from '@rfmap/ui-kit';
import { AltitudeView } from '../core/fpv-altitude';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { Route3dLayer, Route3dScene } from './route-3d-layer';

const TERRAIN_SOURCE = 'fpv-terrain';
const PITCH_3D = 60;
/** MapLibre's default, which the 2D map keeps. */
const MAX_PITCH_2D = 60;

/** The raster-dem source for 3D terrain, from the map config's `dem` (Terrarium or Mapbox encoding). */
export function terrainSource(ref: TileSourceRef) {
  const encoding = ref.encoding ?? 'terrarium';
  const tileSize = ref.tileSize ?? 256;
  if (ref.pmtiles)
    return { type: 'raster-dem' as const, url: `pmtiles://${ref.pmtiles}`, encoding, tileSize };
  return {
    type: 'raster-dem' as const,
    tiles: ref.tiles ?? [],
    encoding,
    tileSize,
    maxzoom: ref.maxzoom ?? 12,
  };
}

/**
 * 3D mode of the map: terrain raised by the vertical exaggeration, pitch and rotation unlocked,
 * and the route drawn at its planned altitude with the safe corridor (fpv.altitude). In 2D mode
 * it draws nothing and puts the camera back flat and north up.
 */
@Component({
  selector: 'fpv-route-3d',
  template: `
    @if (view.mode() === '3d') {
      <div class="panel" role="group" aria-label="3D view">
        <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" aria-label="Back to 2D" (click)="view.setMode('2d')">← 2D</button>
        <label class="ex">
          <span>Heights ×{{ view.exaggeration() }}</span>
          <input
            type="range"
            min="1"
            max="4"
            step="0.5"
            [value]="view.exaggeration()"
            (input)="setExaggeration($event)"
            aria-label="Vertical exaggeration"
          />
        </label>
        <label class="ex">
          <span>Tilt {{ tilt() }}°</span>
          <input type="range" min="0" max="80" step="5" [value]="tilt()" (input)="setTilt($event)" aria-label="Camera tilt" />
        </label>
        <span class="tag" [class.true]="view.exaggeration() === 1">{{
          view.exaggeration() === 1 ? 'true scale' : 'exaggerated ×' + view.exaggeration()
        }}</span>
        @if (!hasTerrain()) {
          <span class="warn">No terrain tiles on this server: the ground is drawn flat.</span>
        }
        @if (!view.corridor()) {
          <button
            type="button"
            class="rf-btn rf-btn--sm rf-btn--secondary"
            (click)="view.profileOpen.set(true)"
          >
            Route heights…
          </button>
        }
      </div>
    }
  `,
  styles: `
    .panel {
      position: absolute;
      z-index: 3;
      left: 14px;
      top: 16px;
      display: flex;
      align-items: center;
      gap: 10px;
      flex-wrap: wrap;
      max-width: calc(100% - 360px);
      padding: 6px 10px;
      border-radius: 8px;
      background: rgba(16, 22, 29, 0.95);
      border: 1px solid var(--rf-border);
      font-size: 12px;
      color: var(--rf-text-2);
    }
    .ex {
      display: inline-flex;
      align-items: center;
      gap: 8px;
    }
    .ex span {
      min-width: 64px;
      color: var(--rf-text);
      font-weight: 600;
    }
    input[type='range'] {
      width: 110px;
    }
    .tag {
      padding: 2px 8px;
      border-radius: 999px;
      background: var(--rf-warn-bg);
      color: var(--rf-warn-text);
    }
    .tag.true {
      background: var(--rf-ok-bg);
      color: var(--rf-ok-text);
    }
    .warn {
      color: var(--rf-warn-text);
    }
    :host-context(.phone) .panel {
      top: 110px;
      max-width: calc(100% - 28px);
    }
  `,
})
export class Route3dComponent {
  protected readonly view = inject(AltitudeView);
  private readonly ws = inject(FpvWorkspace);
  private readonly ctx = inject(MapContext);
  private readonly layer = new Route3dLayer();
  /** Camera tilt in 3D; the map's own pitch gestures are off, so the slider sets it. */
  protected readonly tilt = signal(PITCH_3D);

  protected readonly hasTerrain = computed(() => !!this.ws.data.mapConfig()?.dem);
  private readonly scene = computed(() => {
    const data = this.view.result();
    if (this.view.mode() !== '3d' || !data?.corridor || !data.pilot || data.corridor.s.length < 2)
      return null;
    return new Route3dScene(data, {
      exaggeration: this.view.exaggeration(),
      corridor: this.view.corridorShown(),
      fresnel: this.view.fresnelShown(),
      cursorS: this.view.cursorS(),
    });
  });

  constructor() {
    effect(() => {
      const map = this.ctx.map();
      const mode = this.view.mode();
      if (!map) return;
      untracked(() => (mode === '3d' ? this.enter(map) : this.leave(map)));
    });
    effect(() => {
      const map = this.ctx.map();
      const ex = this.view.exaggeration();
      if (map && this.view.mode() === '3d' && map.getSource(TERRAIN_SOURCE))
        untracked(() => map.setTerrain({ source: TERRAIN_SOURCE, exaggeration: ex }));
    });
    effect(() => {
      const scene = this.scene();
      untracked(() => this.layer.setScene(scene));
    });
    inject(DestroyRef).onDestroy(() => {
      const map = this.ctx.map();
      if (map) this.leave(map, false);
    });
  }

  protected setTilt(e: Event): void {
    const pitch = Number((e.target as HTMLInputElement).value);
    this.tilt.set(pitch);
    this.ctx.map()?.easeTo({ pitch, duration: 200 });
  }

  protected setExaggeration(e: Event): void {
    this.view.exaggeration.set(Number((e.target as HTMLInputElement).value));
  }

  private enter(map: MlMap): void {
    const dem = this.ws.data.mapConfig()?.dem;
    if (dem && !map.getSource(TERRAIN_SOURCE)) map.addSource(TERRAIN_SOURCE, terrainSource(dem));
    if (dem) map.setTerrain({ source: TERRAIN_SOURCE, exaggeration: this.view.exaggeration() });
    if (!map.getLayer(this.layer.id)) map.addLayer(this.layer);
    map.setMaxPitch(80);
    map.dragRotate.enable();
    map.easeTo({ pitch: this.tilt(), duration: 600 });
  }

  private leave(map: MlMap, animate = true): void {
    if (map.getLayer(this.layer.id)) map.removeLayer(this.layer.id);
    if (map.getTerrain()) map.setTerrain(null);
    map.dragRotate.disable();
    map.setMaxPitch(MAX_PITCH_2D);
    if (map.getPitch() !== 0 || map.getBearing() !== 0)
      map.easeTo({ pitch: 0, bearing: 0, duration: animate ? 400 : 0 });
  }
}
