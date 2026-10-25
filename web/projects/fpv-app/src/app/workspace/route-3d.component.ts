import {
  Component,
  DestroyRef,
  ElementRef,
  computed,
  effect,
  inject,
  signal,
  untracked,
  viewChild,
} from '@angular/core';
import { ExpressionSpecification, LayerSpecification, LngLat, LngLatBounds, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import {
  LAYER_GROUP_RANK,
  LayerRegistry,
  MapContext,
  TileSourceRef,
  initialBearingDeg,
} from '@rfmap/ui-kit';
import { AltitudeView } from '../core/fpv-altitude';
import { ChaseCamera, ChaseView } from '../core/fpv-chase';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { ChaseHudComponent } from './chase-hud.component';
import { Route3dCursor, Route3dLayer, Route3dScene } from './route-3d-layer';

const TERRAIN_SOURCE = 'fpv-terrain';
/** A second copy of the elevation source for the relief layers; MapLibre warns when terrain and hillshade share one. */
const RELIEF_SOURCE = 'fpv-relief';
const CONTOUR_SOURCE = 'fpv-contours';
export const RELIEF_LAYERS = ['fpv-relief-colour', 'fpv-relief-shade', 'fpv-contours-minor', 'fpv-contours-major'];
const PITCH_3D = 60;
/** The steepest tilt in 3D, by the user. */
const MAX_PITCH_3D = 80;
/** MapLibre's default, which the 2D map keeps. */
const MAX_PITCH_2D = 60;
const NO_PADDING = { top: 0, bottom: 0, left: 0, right: 0 };
/** The 2D layers that 3D keeps: the 0 dB edge of the coverage (and the battery limit, which 3D leaves out). */
const KEPT_IN_3D = new Set(['results-limits']);
/** 2D layers 3D draws its own way. */
const REPLACED_IN_3D = ['terrain-hillshade', 'terrain-contours'];

/** A dusk sky and fog over the 3D horizon, so a low camera sees the far ground fade instead of a blank. */
export const SKY_3D = {
  'sky-color': '#16202B',
  'horizon-color': '#2E3B4A',
  'fog-color': '#1C2632',
  'sky-horizon-blend': 0.6,
  'horizon-fog-blend': 0.7,
  'fog-ground-blend': 0.85,
  'atmosphere-blend': 0,
};

/** Elevation tints from valley to top, spread over the heights of the route's ground. */
const RELIEF_TINTS = ['#24382C', '#34492F', '#525A35', '#6E6142', '#8A7A5C'];

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

/** The contour lines source (tiles-builder contours.pmtiles, every 20 m, or the dev mock's GeoJSON). */
export function contourSource(ref: TileSourceRef): SourceSpecification {
  if (ref.pmtiles) return { type: 'vector', url: `pmtiles://${ref.pmtiles}` };
  if (ref.tiles) return { type: 'vector', tiles: ref.tiles, maxzoom: ref.maxzoom ?? 14 };
  return { type: 'geojson', data: ref.geojson ?? { type: 'FeatureCollection', features: [] } };
}

/** Colour stops for the elevation tint over [lowM, highM]. */
export function reliefColours(lowM: number, highM: number): ExpressionSpecification {
  const span = Math.max(50, highM - lowM);
  const stops = RELIEF_TINTS.flatMap((c, i) => [lowM + (span * i) / (RELIEF_TINTS.length - 1), c]);
  return ['interpolate', ['linear'], ['elevation'], ...stops] as unknown as ExpressionSpecification;
}

/**
 * The ground of the 3D view: elevation tints with hill shading over them and contour lines
 * (minor every 20 m, darker; index every 100 m, lighter).
 */
export function reliefLayers(contours: TileSourceRef | undefined, lowM: number, highM: number): LayerSpecification[] {
  const layers: LayerSpecification[] = [
    {
      id: 'fpv-relief-colour',
      type: 'color-relief',
      source: RELIEF_SOURCE,
      paint: { 'color-relief-opacity': 0.9, 'color-relief-color': reliefColours(lowM, highM) },
    } as LayerSpecification,
    {
      id: 'fpv-relief-shade',
      type: 'hillshade',
      source: RELIEF_SOURCE,
      paint: {
        'hillshade-shadow-color': 'rgba(4, 7, 11, 0.75)',
        'hillshade-highlight-color': 'rgba(255, 255, 255, 0.25)',
        'hillshade-accent-color': 'rgba(4, 7, 11, 0.3)',
        'hillshade-exaggeration': 0.6,
        'hillshade-illumination-anchor': 'map',
      },
    },
  ];
  if (!contours) return layers;
  const index = ['==', ['%', ['to-number', ['get', 'ele']], 100], 0];
  const vector = !!(contours.pmtiles || contours.tiles);
  const sourceLayer = contours.sourceLayer ?? 'contours';
  layers.push(
    {
      id: 'fpv-contours-minor',
      type: 'line',
      source: CONTOUR_SOURCE,
      ...(vector ? { 'source-layer': sourceLayer } : {}),
      filter: ['!', index],
      paint: { 'line-color': '#0B0F14', 'line-opacity': 0.35, 'line-width': 0.8 },
    } as LayerSpecification,
    {
      id: 'fpv-contours-major',
      type: 'line',
      source: CONTOUR_SOURCE,
      ...(vector ? { 'source-layer': sourceLayer } : {}),
      filter: index,
      paint: { 'line-color': '#D8CFB8', 'line-opacity': 0.45, 'line-width': 1.2 },
    } as LayerSpecification,
  );
  return layers;
}

/**
 * 3D mode of the map: the route at its planned altitude over raised, tinted and shaded terrain
 * (fpv.altitude), seen from the side. The flat 2D route and the coverage heatmap step aside; the
 * 0 dB coverage edge stays on the ground. In 2D mode it draws nothing and puts the camera back
 * flat and north up.
 */
@Component({
  selector: 'fpv-route-3d',
  imports: [ChaseHudComponent],
  template: `
    <div class="labels" #labels aria-hidden="true"></div>
    @if (view.mode() === '3d' && chase.active()) {
      <fpv-chase-hud />
    } @else if (view.mode() === '3d') {
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
        <label class="check">
          <input type="checkbox" [checked]="view.corridor3d()" (change)="view.corridor3d.set($any($event.target).checked)" />
          Corridor
        </label>
        <label class="pick">
          Coverage
          <select [value]="view.coverage3d()" (change)="view.coverage3d.set($any($event.target).value)" aria-label="Coverage in 3D">
            <option value="edge">0 dB edge</option>
          </select>
        </label>
        @if (chase.track()) {
          <button type="button" class="rf-btn rf-btn--sm rf-btn--primary chase" (click)="chase.start()" title="Fly the plan with the camera behind the drone">
            ▶ Chase
          </button>
        }
        @if (!hasTerrain()) {
          <span class="warn">No terrain tiles on this server: the ground is drawn flat.</span>
        }
      </div>
      @if (!view.corridor()) {
        <div class="empty" role="status">
          <b>The route needs its heights to show in 3D.</b>
          <span>Calculate the altitude corridor to see the route over the terrain.</span>
          <button type="button" class="rf-btn rf-btn--sm rf-btn--primary" (click)="calculate()">Calculate heights</button>
        </div>
      }
    }
  `,
  styles: `
    .labels {
      position: absolute;
      inset: 0;
      z-index: 2;
      overflow: hidden;
      pointer-events: none;
    }
    .labels ::ng-deep .fpv-3d-label {
      position: absolute;
      left: 0;
      top: 0;
      margin: -34px 0 0 0;
      translate: -50% 0;
      padding: 2px 6px;
      border-radius: 4px;
      background: rgba(11, 15, 20, 0.86);
      border: 1px solid var(--rf-border);
      color: var(--rf-text);
      font-family: var(--rf-font-mono, 'IBM Plex Mono', monospace);
      font-size: 11px;
      white-space: nowrap;
    }
    .labels ::ng-deep .fpv-3d-label.is-ok,
    .labels ::ng-deep .fpv-3d-label.is-warn {
      margin-top: -58px;
      font-size: 12px;
      z-index: 1;
    }
    .labels ::ng-deep .fpv-3d-label.is-ok {
      border-color: #35d0ba;
      color: #35d0ba;
    }
    .labels ::ng-deep .fpv-3d-label.is-warn {
      border-color: #f5a524;
      color: #f5a524;
    }
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
    .ex,
    .check,
    .pick {
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
    .check input {
      accent-color: var(--rf-ok);
      margin: 0;
    }
    select {
      font: inherit;
      color: var(--rf-text);
      background: var(--rf-surface-2);
      border: 1px solid var(--rf-border);
      border-radius: 6px;
      padding: 2px 6px;
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
    .empty {
      position: absolute;
      z-index: 3;
      left: 50%;
      top: 40%;
      transform: translate(-50%, -50%);
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 8px;
      max-width: min(420px, calc(100% - 28px));
      padding: 16px 20px;
      border-radius: 12px;
      background: rgba(16, 22, 29, 0.95);
      border: 1px solid var(--rf-border);
      text-align: center;
      color: var(--rf-text-2);
      font-size: 13px;
    }
    .empty b {
      color: var(--rf-text);
      font-size: 14px;
    }
    :host-context(.phone) .panel {
      top: 110px;
      max-width: calc(100% - 28px);
    }
    /* clear of the phone's sheet over the lower half of the map */
    :host-context(.phone) .empty {
      top: 26%;
    }
  `,
})
export class Route3dComponent {
  protected readonly view = inject(AltitudeView);
  protected readonly chase = inject(ChaseView);
  private readonly ws = inject(FpvWorkspace);
  private readonly ctx = inject(MapContext);
  private readonly layers = inject(LayerRegistry);
  private readonly layer = new Route3dLayer();
  private readonly labelHost = viewChild.required<ElementRef<HTMLElement>>('labels');
  /** Camera tilt in 3D; the map's own pitch gestures are off, so the slider sets it. */
  protected readonly tilt = signal(PITCH_3D);

  protected readonly hasTerrain = computed(() => !!this.ws.data.mapConfig()?.dem);
  /** Heights of the route's ground, which the elevation tints spread over. */
  private readonly groundRange = computed(() => {
    const g = this.view.corridor()?.c.groundMslM;
    if (!g?.length) return { low: 100, high: 600 };
    return { low: Math.min(...g) - 20, high: Math.max(...g) + 60 };
  });
  /** The data to draw in 3D, or null outside 3D or without a corridor. */
  private readonly data3d = computed(() => {
    const data = this.view.result();
    if (this.view.mode() !== '3d' || !data?.corridor || !data.pilot || data.corridor.s.length < 2)
      return null;
    return data;
  });
  private readonly scene = computed(() => {
    const data = this.data3d();
    return data
      ? new Route3dScene(data, { exaggeration: this.view.exaggeration(), corridor: this.view.corridor3d() })
      : null;
  });
  /** The cursor on its own, so moving it leaves the route's buffers alone. */
  private readonly cursor = computed(() => {
    const data = this.data3d();
    const cursorS = this.view.cursorS();
    if (!data || cursorS === null) return null;
    return new Route3dCursor(data, { exaggeration: this.view.exaggeration(), fresnel: this.view.fresnelShown(), cursorS });
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
      const map = this.ctx.map();
      const { low, high } = this.groundRange();
      if (map?.getLayer('fpv-relief-colour'))
        untracked(() => map.setPaintProperty('fpv-relief-colour', 'color-relief-color', reliefColours(low, high)));
    });
    // a route calculated while in 3D: frame it from the side once its heights arrive
    let framed = false;
    effect(() => {
      const map = this.ctx.map();
      const has = !!this.view.corridor();
      if (this.view.mode() !== '3d') framed = false;
      else if (map && has && !framed) {
        framed = true;
        untracked(() => this.frame(map));
      }
    });
    effect(() => {
      const scene = this.scene();
      untracked(() => this.layer.setScene(scene));
    });
    effect(() => {
      const cursor = this.cursor();
      untracked(() => this.layer.setCursor(cursor));
    });
    effect(() => this.layer.setLabelHost(this.labelHost().nativeElement));
    // the chase camera: behind the drone on every frame, and back to the side view when it ends
    let chasing = false;
    effect(() => {
      const map = this.ctx.map();
      const track = this.chase.track();
      const on = this.chase.active() && this.view.mode() === '3d';
      const s = this.chase.s();
      const ex = this.view.exaggeration();
      if (!map) return;
      untracked(() => {
        if (on && track) this.follow(map, track.camera(s, ex));
        else if (chasing) {
          // the chase's padding stays on the map until it is put back
          map.setPadding(NO_PADDING);
          if (this.view.mode() === '3d') this.frame(map);
        }
      });
      chasing = on && !!track;
    });
    // leaving 3D, or a result without a corridor, ends the chase
    effect(() => {
      if (this.chase.active() && (this.view.mode() !== '3d' || !this.chase.track())) untracked(() => this.chase.stop());
    });
    inject(DestroyRef).onDestroy(() => {
      const map = this.ctx.map();
      if (map) this.leave(map, false);
      this.layer.setLabelHost(null);
    });
  }

  private follow(map: MlMap, cam: ChaseCamera): void {
    const opts = map.calculateCameraOptionsFromTo(
      new LngLat(cam.from.lon, cam.from.lat),
      cam.fromAltM,
      new LngLat(cam.to.lon, cam.to.lat),
      cam.toAltM,
    );
    // the drawer under the map hides its lower part: aim at the middle of what shows
    const cover = Number.parseFloat(getComputedStyle(map.getContainer()).getPropertyValue('--fpv-cover-bottom')) || 0;
    map.jumpTo({ ...opts, padding: { top: 0, left: 0, right: 0, bottom: cover } });
  }

  protected setTilt(e: Event): void {
    const pitch = Number((e.target as HTMLInputElement).value);
    this.tilt.set(pitch);
    this.ctx.map()?.easeTo({ pitch, duration: 200 });
  }

  protected setExaggeration(e: Event): void {
    this.view.exaggeration.set(Number((e.target as HTMLInputElement).value));
  }

  protected calculate(): void {
    void this.view.enable();
  }

  private enter(map: MlMap): void {
    this.raiseTerrain(map);
    this.layers.suppress(this.hiddenIn3d());
    map.getContainer().classList.add('fpv-3d');
    map.setSky(SKY_3D);
    if (!map.getLayer(this.layer.id)) map.addLayer(this.layer);
    map.setMaxPitch(MAX_PITCH_3D);
    map.dragRotate.enable();
    if (this.view.corridor()) this.frame(map);
    else map.easeTo({ pitch: this.tilt(), duration: 600 });
  }

  /** Raised terrain with its tints, shading and contours under the other layers, when the server has terrain tiles. */
  private raiseTerrain(map: MlMap): void {
    const cfg = this.ws.data.mapConfig();
    const dem = cfg?.dem;
    if (!dem) return;
    if (!map.getSource(TERRAIN_SOURCE)) map.addSource(TERRAIN_SOURCE, terrainSource(dem));
    if (!map.getSource(RELIEF_SOURCE)) map.addSource(RELIEF_SOURCE, terrainSource(dem));
    if (cfg.contours && !map.getSource(CONTOUR_SOURCE)) map.addSource(CONTOUR_SOURCE, contourSource(cfg.contours));
    const before = this.firstLayerAboveTerrain(map);
    const { low, high } = this.groundRange();
    for (const l of reliefLayers(cfg.contours, low, high)) if (!map.getLayer(l.id)) map.addLayer(l, before);
    map.setTerrain({ source: TERRAIN_SOURCE, exaggeration: this.view.exaggeration() });
  }

  private leave(map: MlMap, animate = true): void {
    if (map.getLayer(this.layer.id)) map.removeLayer(this.layer.id);
    for (const id of [...RELIEF_LAYERS].reverse()) if (map.getLayer(id)) map.removeLayer(id);
    for (const id of [RELIEF_SOURCE, CONTOUR_SOURCE]) if (map.getSource(id)) map.removeSource(id);
    if (map.getTerrain()) map.setTerrain(null);
    this.layers.suppress([]);
    map.getContainer().classList.remove('fpv-3d');
    map.setSky({});
    map.setPadding(NO_PADDING);
    map.dragRotate.disable();
    map.setMaxPitch(MAX_PITCH_2D);
    if (map.getPitch() !== 0 || map.getBearing() !== 0)
      map.easeTo({ pitch: 0, bearing: 0, duration: animate ? 400 : 0 });
  }

  /** The route seen from the side: the camera faces across the line from the pilot to the target. */
  private frame(map: MlMap): void {
    const m = this.ws.mission();
    const points = m ? [m.pilot, ...m.waypoints] : [];
    if (points.length < 2) {
      map.easeTo({ pitch: this.tilt(), duration: 600 });
      return;
    }
    const last = points[points.length - 1];
    const bearing = (((initialBearingDeg(m!.pilot, last) - 90) % 360) + 540) % 360 - 180;
    const bounds = new LngLatBounds();
    for (const p of points) bounds.extend([p.lon, p.lat]);
    const cam = map.cameraForBounds(bounds, { bearing, pitch: this.tilt(), padding: { top: 140, bottom: 120, left: 80, right: 80 } });
    map.easeTo({ ...(cam ?? {}), bearing, pitch: this.tilt(), duration: 800 });
  }

  /** The flat 2D layers 3D hides: the route, the heatmap and other results (all but the 0 dB edge), and the terrain it redraws. */
  private hiddenIn3d(): string[] {
    const flat = this.layers
      .layers()
      .filter((e) => (e.def.group === 'mission' || e.def.group === 'results') && !KEPT_IN_3D.has(e.def.id))
      .map((e) => e.def.id);
    return [...flat, ...REPLACED_IN_3D];
  }

  /** The first style layer of the weather, results or mission groups, so the relief goes under them. */
  private firstLayerAboveTerrain(map: MlMap): string | undefined {
    const above = new Set(
      this.layers
        .layers()
        .filter((e) => LAYER_GROUP_RANK[e.def.group] >= LAYER_GROUP_RANK.weather)
        .flatMap((e) => e.def.styleLayerIds()),
    );
    return map.getLayersOrder().find((id) => above.has(id));
  }
}
