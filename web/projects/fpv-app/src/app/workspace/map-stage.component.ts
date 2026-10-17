import { Component, DestroyRef, computed, effect, inject, input, signal, untracked } from '@angular/core';
import maplibregl, { Map as MlMap, Marker } from 'maplibre-gl';
import type { Geometry, Position } from 'geojson';
import {
  CoverageDeltaLayer,
  CoverageLayer,
  DrawMarker,
  RouteDeltaLayer,
  DrawPoint,
  LayerPanelComponent,
  LayerRegistry,
  LegDrawStatus,
  LimitsLayer,
  MapContext,
  MapViewComponent,
  MissionPoint,
  RfFormat,
  RouteDrawing,
  RouteTool,
  SegmentedComponent,
  SlotHostComponent,
  WeatherLayer,
  HeatLegendComponent,
  createStandardLayers,
  destinationPoint,
  distanceM,
} from '@rfmap/ui-kit';
import { FpvWorkspace, LINK_LETTER, endsAtTurnBack } from '../core/fpv-workspace.service';
import { CompareService } from '../core/fpv-core';

/** The map with everything FPV draws on it: route, markers, coverage, limits, preview. */
@Component({
  selector: 'fpv-map-stage',
  imports: [MapViewComponent, LayerPanelComponent, SlotHostComponent, SegmentedComponent, HeatLegendComponent],
  template: `
    @if (ws.data.mapConfig(); as cfg) {
      <rf-map-view class="map" [config]="cfg" label="Mission map" (mapReady)="onReady($event)">
        <div class="overlay top-right">
          <button type="button" class="rf-btn rf-btn--sm map-btn" [attr.aria-expanded]="ws.layersOpen()" (click)="ws.layersOpen.set(!ws.layersOpen())">
            <span aria-hidden="true">◫</span> Layers
          </button>
          @if (ws.layersOpen()) {
            <rf-layer-panel
              [forecastHours]="48"
              [forecastOffset]="ws.forecastOffsetH()"
              [forecastTime]="forecastTime()"
              (forecastOffsetChange)="ws.forecastOffsetH.set($event)"
              (closed)="ws.layersOpen.set(false)"
            />
          }
          @if (cmp.delta() || routeDeltaShown()) {
            <div class="legend" role="note" aria-label="Scenario difference legend">
              <div class="legend-head"><span>{{ (cmp.options()?.name || 'B') === 'B' ? 'B' : 'B “' + cmp.options()?.name + '”' }} − A · {{ cmp.link() }} link</span></div>
              <div class="delta-scale" aria-hidden="true"></div>
              <div class="delta-keys"><span>▼ B worse</span><span>= under 1 dB</span><span>▲ B better</span></div>
            </div>
          } @else if (showHeatLegend()) {
            <div class="legend">
              <div class="legend-head">
                <span>Coverage at {{ droneAgl() }} m AGL</span>
                @if (ws.evaluated().length > 1) {
                  <rf-segmented label="Coverage link" [options]="linkOptions" [value]="ws.heatLink()" (valueChange)="ws.coverageLink.set($event)" />
                }
              </div>
              <rf-heat-legend />
            </div>
          }
        </div>
        @if (ws.store.preview()) {
          <div class="overlay banner" role="status">Preview on the map · Apply to keep</div>
        }
        @if (ws.placing(); as p) {
          <div class="overlay hint" role="status">
            {{ p === 'pilot' ? 'Click the map to place the pilot' : 'Click the map to add a point · the last point is the target' }}
            @if (p === 'waypoints') {
              <button type="button" class="rf-btn rf-btn--sm rf-btn--primary" (click)="ws.placing.set(null)" [disabled]="(ws.mission()?.waypoints?.length ?? 0) === 0">Done</button>
            }
          </div>
        }
        @if (ws.editingRoute() && !ws.placing()) {
          <div class="overlay hint" role="status">Drag a point to move it · coverage is from before the edit</div>
        }
        <rf-slot-host class="overlay slot" name="map.overlays" />
      </rf-map-view>
      @if (dim()) {
        <div class="dim" aria-hidden="true"></div>
      }
    }
  `,
  styles: `
    :host { position: relative; display: block; overflow: hidden; }
    .map { position: absolute; inset: 0; }
    .overlay { position: absolute; z-index: 3; }
    .top-right { top: 16px; right: 16px; display: flex; flex-direction: column; align-items: flex-end; gap: 8px; max-height: calc(100% - 32px); }
    .map-btn { background: rgba(16, 22, 29, 0.95); border: 1px solid var(--rf-border); }
    .delta-scale { height: 10px; border-radius: 5px; background: linear-gradient(90deg, #FF6B5B, #8A96A5 50%, #35D0BA); }
    .delta-keys { display: flex; justify-content: space-between; font-size: 11px; }
    .legend { width: 240px; padding: 10px 12px; border-radius: var(--rf-radius-lg); background: rgba(16, 22, 29, 0.95); border: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 8px; font-size: 12px; color: var(--rf-text-2); }
    .legend-head { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
    .legend rf-segmented { width: 84px; }
    .legend ::ng-deep .rf-segmented > button { height: 26px; font-size: 12px; }
    .banner { top: 16px; left: 50%; transform: translateX(-50%); padding: 6px 14px; border-radius: 999px; background: var(--rf-video-bg); color: var(--rf-video-text); font-weight: 600; font-size: 13px; white-space: nowrap; }
    .hint { bottom: 44px; left: 14px; padding: 8px 12px; border-radius: 8px; background: rgba(16, 22, 29, 0.95); border: 1px solid var(--rf-border); font-size: 13px; color: var(--rf-text-2); display: flex; gap: 10px; align-items: center; }
    .slot { left: 14px; top: 16px; }
    .dim { position: absolute; inset: 0; background: rgba(11, 15, 20, 0.72); z-index: 4; pointer-events: none; }
    :host-context(.phone) .top-right { top: 64px; right: 12px; }
    :host-context(.phone) .legend { display: none; }
  `,
})
export class MapStageComponent {
  protected readonly ws = inject(FpvWorkspace);
  private readonly layers = inject(LayerRegistry);
  private readonly ctx = inject(MapContext);
  private readonly destroyRef = inject(DestroyRef);

  /** Map padding for fitting the route (drawer and sheets cover parts of the map). */
  readonly fitPadding = input<{ top: number; bottom: number; left: number; right: number }>({ top: 80, bottom: 80, left: 80, right: 80 });
  readonly interactiveEdit = input(true);
  /** Height of the bottom drawer that covers the map while "why" is open (desktop); 0 = none. */
  readonly drawerHeight = input(0);

  protected readonly linkOptions = [
    { value: 'control' as const, label: 'C' },
    { value: 'video' as const, label: 'V' },
  ];

  private tool?: RouteTool;
  private coverage = new CoverageLayer();
  private limits = new LimitsLayer();
  private coverageDelta = new CoverageDeltaLayer();
  private routeDelta = new RouteDeltaLayer();
  protected readonly cmp = inject(CompareService);
  private weather: WeatherLayer[] = [];
  private tags: Marker[] = [];
  private readonly ready = signal(false);
  private fittedFor: string | null = null;

  protected readonly dim = computed(() => !this.ws.mission() && !this.ws.placing());
  protected readonly routeDeltaShown = computed(() => !!this.cmp.result()?.links?.[this.cmp.link()]?.points?.length && !this.ws.store.preview());
  protected readonly droneAgl = computed(() => this.ws.mission()?.coverage?.droneAglM ?? 120);
  protected readonly showHeatLegend = computed(() => !!this.ws.store.coverage()[this.ws.heatLink()] && this.layers.isVisible('results-coverage'));
  protected readonly forecastTime = computed(() => {
    const base = new Date();
    base.setMinutes(0, 0, 0);
    return new Date(base.getTime() + this.ws.forecastOffsetH() * 3600e3).toISOString();
  });

  constructor() {
    effect(() => {
      if (!this.ready()) return;
      const d = this.drawing();
      untracked(() => this.tool?.draw(d));
    });
    effect(() => {
      if (!this.ready()) return;
      const editable = this.ws.editingRoute() && this.ws.store.canEdit() && !this.ws.store.preview();
      untracked(() => this.tool?.setEditable(editable));
    });
    effect(() => {
      if (!this.ready()) return;
      // while the B − A difference map is shown, it replaces the plain heatmap
      const grid = this.cmp.delta() ? null : (this.ws.store.coverage()[this.ws.heatLink()] ?? null);
      untracked(() => this.coverage.setGrid(grid));
    });
    effect(() => {
      if (!this.ready()) return;
      const delta = this.cmp.delta();
      untracked(() => this.coverageDelta.setDelta(delta));
    });
    effect(() => {
      if (!this.ready()) return;
      const pts = this.ws.store.preview() ? null : (this.cmp.result()?.links?.[this.cmp.link()]?.points ?? null);
      untracked(() => this.routeDelta.setPoints(pts));
    });
    effect(() => {
      if (!this.ready()) return;
      const shapes = this.limitShapes();
      untracked(() => {
        this.limits.setShapes(shapes);
        this.drawTags(shapes);
      });
    });
    effect(() => {
      const t = this.forecastTime();
      untracked(() => this.weather.forEach((w) => w.setTime(t)));
    });
    effect(() => {
      if (!this.ready()) return;
      const placing = this.ws.placing();
      untracked(() =>
        this.tool?.setAddPointHandler(
          placing
            ? (p) => {
                if (placing === 'pilot') {
                  const pending = this.ws.pendingMission();
                  if (!this.ws.mission() && pending) {
                    this.ws.store.newMission({ ...pending, pilot: { ...pending.pilot, lat: p.lat, lon: p.lon } });
                    this.ws.pendingMission.set(null);
                  } else this.ws.store.setPilot(p);
                  this.ws.placing.set('waypoints');
                } else {
                  this.ws.store.addWaypoint({ lat: p.lat, lon: p.lon, aglM: 120 });
                }
              }
            : null,
        ),
      );
    });
    // fit the camera when a different mission is shown
    effect(() => {
      if (!this.ready()) return;
      const m = this.ws.mission();
      const key = m ? `${this.ws.store.missionId() ?? 'new'}:${m.name}:${m.waypoints.length > 0}` : null;
      if (!m || key === this.fittedFor || m.waypoints.length === 0) return;
      this.fittedFor = key;
      untracked(() => this.ctx.fitTo([m.pilot, ...m.waypoints], this.fitPadding()));
    });
    // keep the route visible above the why drawer
    let drawerOpen = false;
    effect(() => {
      if (!this.ready()) return;
      const open = !!this.ws.why() && this.drawerHeight() > 0;
      if (open === drawerOpen) return;
      drawerOpen = open;
      const m = untracked(() => this.ws.mission());
      if (!m || m.waypoints.length === 0) return;
      const pad = this.fitPadding();
      untracked(() => this.ctx.fitTo([m.pilot, ...m.waypoints], open ? { ...pad, top: 40, bottom: this.drawerHeight() + 60 } : pad));
    });
    this.destroyRef.onDestroy(() => {
      this.tool?.destroy();
      this.tags.forEach((t) => t.remove());
    });
  }

  onReady(map: MlMap): void {
    const cfg = this.ws.data.mapConfig();
    if (cfg) {
      for (const def of createStandardLayers(cfg, this.forecastTime())) {
        this.layers.register(def);
        if (def instanceof WeatherLayer) this.weather.push(def);
      }
    }
    this.layers.register(this.coverage);
    this.layers.register(this.limits);
    this.layers.register(this.coverageDelta);
    this.layers.register(this.routeDelta);
    this.tool = new RouteTool(this.layers, this.ctx, {
      onMove: (i, p) => this.ws.store.movePoint(i, p),
      onSelectLeg: (i) => {
        if (this.ws.store.result()) this.ws.openWhy(i + 1);
      },
    });
    void map;
    this.ready.set(true);
  }

  /** Route drawing for the shown mission (the preview mission while previewing). */
  private readonly drawing = computed<RouteDrawing>(() => {
    const m = this.ws.shown();
    if (!m) return { pilot: null, waypoints: [], legs: [] };
    const preview = this.ws.store.preview();
    const before = preview?.before;
    const rows = this.ws.legRows();
    const ground = this.ws.groundAt();
    const g = (v: number | undefined, agl: number) => (v !== undefined ? `${RfFormat.format(v, 'm', { unit: false })} + ${RfFormat.format(agl, 'm')}` : `${RfFormat.format(agl, 'm')} AGL`);
    const turn = endsAtTurnBack(m);
    const pilot: DrawPoint = { ...m.pilot, caption: `Pilot · ${g(ground.pilot, m.pilot.aglM)}`, ariaLabel: `Pilot A, ${RfFormat.format(m.pilot.aglM, 'm')} above ground` };
    const waypoints: DrawPoint[] = m.waypoints.map((w, i) => {
      const last = i === m.waypoints.length - 1;
      if (last && turn) return { ...w, markerKind: 'turnback', symbol: 'T', ariaLabel: 'Turn-back point T' };
      return {
        ...w,
        caption: last ? `Target · ${g(ground.target, w.aglM)}` : undefined,
        ariaLabel: last ? `Target B, ${RfFormat.format(w.aglM, 'm')} above ground` : `Waypoint ${i + 1}, ${RfFormat.format(w.aglM, 'm')} above ground`,
      };
    });
    const legs = rows
      .filter((r) => !r.isReturn)
      .map((r, i) => {
        const changed = !!before && !samePoint(before.waypoints[i], m.waypoints[i], before, m, i);
        let status: LegDrawStatus = r.tone === 'blocked' ? 'blocked' : r.tone === 'warn' ? 'missing' : r.tone === 'ok' ? 'ok' : 'pending';
        if (preview) {
          const prevRow = this.previewBaseStatus(i);
          status = changed ? 'preview' : prevRow;
        }
        const km = RfFormat.format(r.lengthM, 'km1');
        let label: string | undefined = r.mark ? `${km} · ${r.mark}` : km;
        if (changed) label = `${km} · ${RfFormat.format(r.aglM, 'm')}`;
        else if (!preview && r.tone === 'ok' && before === undefined && this.ws.store.result() && m.waypoints[i]?.aglM !== 120) label = `${km} · ${RfFormat.format(r.aglM, 'm')} ✓`;
        if (preview && !changed) label = this.previewBaseLabel(i) ?? label;
        return { status, label };
      });
    const markers: DrawMarker[] = [];
    const v = this.ws.verdict();
    if (!preview && v) {
      const dom = this.ws.store.result()?.legs?.find((l) => l.index === v.legIndex)?.worst?.[v.link]?.diffraction;
      const d = dom?.dominant;
      if (d?.lat !== undefined && d.lon !== undefined) {
        markers.push({ kind: 'obstruction', symbol: '✕', lat: d.lat, lon: d.lon, caption: `Ridge ${RfFormat.format(dom?.edgeHeightM ?? d.groundMslM, 'm')}`, ariaLabel: `Obstruction ${RfFormat.format(dom?.edgeHeightM ?? d.groundMslM, 'm')}` });
      }
    }
    const ret = turn && m.returnToPilot !== false ? { from: m.waypoints[m.waypoints.length - 1], label: `Turn back · ${RfFormat.format(rows.find((r) => r.isReturn)?.lengthM, 'km1')}` } : null;
    return { pilot, waypoints, legs, markers, returnLeg: ret };
  });

  private previewBaseStatus(i: number): LegDrawStatus {
    const before = this.ws.store.result()?.legs?.[i];
    if (!before) return 'pending';
    const failing = (['control', 'video'] as const).some((k) => before.worst?.[k]?.reachable === false);
    return failing ? 'blocked' : before.status === 'missing-data' ? 'missing' : 'ok';
  }

  private previewBaseLabel(i: number): string | undefined {
    const before = this.ws.store.result()?.legs?.[i];
    if (!before?.lengthM) return undefined;
    const failing = (['control', 'video'] as const).filter((k) => before.worst?.[k]?.reachable === false);
    return `${RfFormat.format(before.lengthM, 'km1')} · ${failing.length ? failing.map((k) => LINK_LETTER[k]).join(' ') + ' ✕' : '✓'}`;
  }

  private readonly limitShapes = computed(() => {
    const m = this.ws.mission();
    const r = this.ws.store.result();
    const grid = this.ws.store.coverage()[this.ws.heatLink()];
    const preview = this.ws.store.preview();
    const turnPreview = !!preview && endsAtTurnBack(preview.after);
    const radio = (grid?.limitPolygon as Geometry | undefined) ?? null;
    let radioKm: number | null = null;
    let radioAnchor: Position | null = null;
    if (radio && m) {
      const coords = flatCoords(radio);
      let best = -Infinity;
      for (const c of coords) {
        const d = distanceM(m.pilot, { lat: c[1], lon: c[0] });
        if (d > (radioKm ?? 0)) radioKm = d;
        const score = -c[0] + c[1] * 0.3; // westernmost-north point for the label
        if (score > best) {
          best = score;
          radioAnchor = c;
        }
      }
    }
    const b = r?.battery;
    const battery = m && b?.rangeM && !turnPreview && !endsAtTurnBack(m) ? { lat: m.pilot.lat, lon: m.pilot.lon, radiusM: b.rangeM / 2 } : null;
    return {
      radio,
      radioLabel: radioKm ? `${this.ws.heatLink() === 'control' ? 'Control' : 'Video'} link limit · ${RfFormat.format(radioKm, 'km1')}` : '',
      radioAnchor,
      battery,
      batteryLabel: battery ? `Battery return limit · ${RfFormat.format(battery.radiusM, 'km1')}` : '',
    };
  });

  private drawTags(s: { radioLabel: string; radioAnchor: Position | null; battery: { lat: number; lon: number; radiusM: number } | null; batteryLabel: string }): void {
    const map = this.ctx.map();
    this.tags.forEach((t) => t.remove());
    this.tags = [];
    if (!map) return;
    const tag = (text: string, cls: string, lon: number, lat: number, anchor: 'bottom' | 'top' | 'right') => {
      const el = document.createElement('div');
      el.className = `rf-map-tag ${cls}`;
      el.textContent = text;
      this.tags.push(new maplibregl.Marker({ element: el, anchor }).setLngLat([lon, lat]).addTo(map));
    };
    if (s.radioLabel && s.radioAnchor && this.layers.isVisible('results-limits')) tag(s.radioLabel, '', s.radioAnchor[0], s.radioAnchor[1], 'right');
    if (s.battery && this.layers.isVisible('results-limits')) {
      const p = destinationPoint(s.battery.lat, s.battery.lon, 160, s.battery.radiusM);
      tag(s.batteryLabel, 'rf-map-tag--warn', p.lon, p.lat, 'top');
    }
  }
}

function samePoint(a: MissionPoint | undefined, b: MissionPoint | undefined, _bm: unknown, _am: unknown, _i: number): boolean {
  if (!a || !b) return a === b;
  return a.lat === b.lat && a.lon === b.lon && a.aglM === b.aglM && a.label === b.label;
}

function flatCoords(g: Geometry): Position[] {
  switch (g.type) {
    case 'Polygon':
      return g.coordinates.flat();
    case 'MultiPolygon':
      return g.coordinates.flat(2);
    case 'LineString':
      return g.coordinates;
    case 'MultiLineString':
      return g.coordinates.flat();
    default:
      return [];
  }
}
