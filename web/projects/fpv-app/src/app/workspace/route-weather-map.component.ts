import { Component, DestroyRef, computed, effect, inject, input, signal, untracked } from '@angular/core';
import { MapMouseEvent, Map as MlMap, Marker } from 'maplibre-gl';
import {
  LatLon,
  LayerRegistry,
  MapContext,
  PolylineHitTest,
  RfFormat,
  RouteTimeline,
  RouteWeather,
  RouteWeatherLayer,
  WeatherMetric,
  WeatherReading,
  WeatherRibbonSegment,
  WeatherScale,
  Wind,
} from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { FlightClock, KeyPointWeatherVm, RouteWeatherService } from '../core/route-weather.service';
import { RouteColoursComponent } from '../panels/route-colours.component';

/** Read-out of the weather where the cursor (or a tap) meets the route. */
interface RouteHover {
  x: number;
  y: number;
  title: string;
  lines: string[];
}

/** Pixels from the route within which hovering (or tapping, on phones) reads the weather. */
const HOVER_PX = 12;
const TAP_PX = 22;

/** Splits the route between forecast samples into coloured pieces, broken at every route point. */
export class WeatherRibbon {
  constructor(
    private readonly timeline: RouteTimeline,
    private readonly route: RouteWeather,
  ) {}

  segments(metric: WeatherMetric): WeatherRibbonSegment[] {
    const readings = this.route.readings();
    const out: WeatherRibbonSegment[] = [];
    for (let i = 0; i + 1 < readings.length; i++) {
      const cuts = this.cuts(readings[i].alongM, readings[i + 1].alongM);
      for (let j = 0; j + 1 < cuts.length; j++) out.push(this.piece(cuts[j], cuts[j + 1], metric));
    }
    return out;
  }

  /** `from`, the route points strictly between, and `to`. */
  private cuts(from: number, to: number): number[] {
    const inner: number[] = [];
    for (let leg = 1; leg < this.timeline.legCount; leg++) {
      const at = this.timeline.legStartM(leg);
      if (at > from && at < to) inner.push(at);
    }
    return [from, ...inner, to];
  }

  private piece(a: number, b: number, metric: WeatherMetric): WeatherRibbonSegment {
    const mid = this.route.readingAt((a + b) / 2);
    const v = mid ? WeatherScale.value(metric, mid) : undefined;
    return { from: this.at(a), to: this.at(b), color: WeatherScale.color(metric, v) };
  }

  private at(alongM: number): LatLon {
    const p = this.timeline.pointAt(alongM);
    return { lat: p.lat, lon: p.lon };
  }
}

/**
 * Weather mode on the map: the route recoloured by the chosen value with its legend, a weather tag
 * at each route point, and a read-out where the cursor (or a tap) meets the route. Outside weather
 * mode it draws nothing.
 */
@Component({
  selector: 'fpv-route-weather-map',
  imports: [RouteColoursComponent],
  template: `
    @if (shown()) {
      @if (compact()) {
        <div class="keys" role="group" [attr.aria-label]="'Route coloured by ' + title()">
          <b>{{ title() }}</b>
          @for (k of legend(); track k.label) {
            <span><i [style.background]="k.color"></i>{{ k.label }}</span>
          }
        </div>
      } @else {
        <fpv-route-colours class="metric" role="group" aria-label="Route colours in weather mode" />
      }
    }
    @if (active()) {
      @if (hover(); as h) {
        <div class="hover" role="status" [style.left.px]="h.x" [style.top.px]="h.y">
          <b>{{ h.title }}</b>
          @for (l of h.lines; track $index) {
            <span>{{ l }}</span>
          }
        </div>
      }
    }
  `,
  styles: `
    :host { position: absolute; inset: 0; pointer-events: none; z-index: 2; }
    .metric { position: absolute; left: 14px; top: 16px; pointer-events: auto; padding: 8px; border-radius: var(--rf-radius-lg); background: rgba(16, 22, 29, 0.95); border: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 6px; }

    .keys { position: absolute; left: 10px; right: 64px; bottom: 44px; padding: 6px 8px; border-radius: var(--rf-radius-md); background: rgba(16, 22, 29, 0.95); border: 1px solid var(--rf-border); display: flex; flex-wrap: wrap; align-items: center; gap: 3px 10px; font-size: 11px; color: var(--rf-text-2); }
    .keys b { color: var(--rf-text); font-weight: 600; }
    .keys span { display: inline-flex; align-items: center; gap: 4px; }
    .keys i { width: 14px; height: 5px; border-radius: 2px; }

    .hover { position: absolute; transform: translate(14px, 14px); max-width: 280px; padding: 8px 10px; border-radius: var(--rf-radius-md); background: rgba(16, 22, 29, 0.97); border: 1px solid var(--rf-border-strong); display: flex; flex-direction: column; gap: 3px; font-size: 12px; font-family: var(--rf-font-mono); color: var(--rf-text); }
    .hover b { font-family: var(--rf-font-sans); font-size: 12px; color: var(--rf-text-2); font-weight: 500; }
  `,
})
export class RouteWeatherMapComponent {
  private readonly ws = inject(FpvWorkspace);
  private readonly wx = inject(RouteWeatherService);
  private readonly layers = inject(LayerRegistry);
  private readonly ctx = inject(MapContext);
  /** Phone: the metric picker sits in the weather card, the map keeps a one-line legend and short tags; tap instead of hover */
  readonly compact = input(false);

  protected readonly hover = signal<RouteHover | null>(null);
  protected readonly title = computed(() => WeatherScale.title(this.ws.weatherMetric()));
  protected readonly legend = computed(() => WeatherScale.legend(this.ws.weatherMetric()));

  private readonly layer = new RouteWeatherLayer();
  private tags: Pick<Marker, 'remove'>[] = [];

  /** Weather mode with a route, outside a fix preview (the preview draws its own route). */
  protected readonly shown = computed(() => this.ws.weatherMode() && !this.ws.store.preview() && this.wx.timeline() !== null);

  /** Shown, with forecasts on the route to colour it by. */
  protected readonly active = computed(() => {
    const route = this.wx.route();
    return this.shown() && route !== null && !route.empty;
  });


  constructor() {
    const off = this.layers.register(this.layer);
    effect(() => {
      const segments = this.active() ? this.ribbon() : [];
      untracked(() => this.layer.setSegments(segments));
    });
    effect(() => {
      const map = this.ctx.map();
      const points = this.active() ? this.wx.keyPoints() : [];
      const metric = this.ws.weatherMetric();
      const compact = this.compact();
      untracked(() => this.drawTags(map, points, compact ? metric : null));
    });
    effect(() => {
      if (!this.active()) untracked(() => this.hover.set(null));
    });
    const offMap = this.listen();
    const offClick = this.ctx.onClick((p) => this.onTap(p));
    inject(DestroyRef).onDestroy(() => {
      off();
      offMap();
      offClick();
      this.drawTags(null, [], null);
    });
  }

  private ribbon(): WeatherRibbonSegment[] {
    const t = this.wx.timeline();
    const r = this.wx.route();
    return t && r ? new WeatherRibbon(t, r).segments(this.ws.weatherMetric()) : [];
  }

  /** Mouse move and leave on the map, attached once the map exists. */
  private listen(): () => void {
    let attached: MlMap | null = null;
    const move = (e: MapMouseEvent) => this.hover.set(this.readAt(e.point, HOVER_PX));
    const leave = () => this.hover.set(null);
    const ref = effect(() => {
      const map = this.ctx.map();
      if (!map || map === attached) return;
      attached = map;
      map.on('mousemove', move);
      map.on('mouseout', leave);
    });
    return () => {
      ref.destroy();
      attached?.off('mousemove', move);
      attached?.off('mouseout', leave);
    };
  }

  /** A tap (phones) pins the read-out; a tap away from the route clears it. */
  private onTap(p: LatLon): void {
    const map = this.ctx.map();
    if (!map || !this.compact()) return;
    const pt = map.project([p.lon, p.lat]);
    this.hover.set(this.readAt(pt, TAP_PX));
  }

  private readAt(cursor: { x: number; y: number }, tolerance: number): RouteHover | null {
    const map = this.ctx.map();
    const t = this.wx.timeline();
    const r = this.wx.route();
    if (!map || !t || !r || !this.active()) return null;
    const screen = t.stops.map((s) => map.project([s.lon, s.lat]));
    const hit = new PolylineHitTest(screen).nearest(cursor, tolerance);
    if (!hit) return null;
    const start = t.legStartM(hit.segment);
    const along = start + (t.legStartM(hit.segment + 1) - start) * hit.t;
    const reading = r.readingAt(along);
    return reading ? { x: cursor.x, y: cursor.y, ...this.describe(t, reading) } : null;
  }

  private describe(t: RouteTimeline, w: WeatherReading): Pick<RouteHover, 'title' | 'lines'> {
    const leg = `${t.stops[w.leg].name} → ${t.stops[w.leg + 1]?.name ?? ''}`;
    const wind = w.windSpeedMps === undefined ? 'wind —' : `${RfFormat.format(w.windSpeedMps, 'mps')} from ${Wind.compass(w.windFromDeg ?? 0)}`;
    const along = w.alongTrackMps === undefined ? '' : ` · ${WeatherScale.format('headwind', w.alongTrackMps)}`;
    const rh = w.relativeHumidity === undefined ? '' : ` · ${Math.round(w.relativeHumidity)}% RH`;
    return {
      title: `Leg ${w.leg + 1} · ${leg} · ${RfFormat.format(w.alongM, 'km1')} from ${t.stops[0].name} · ETA ${FlightClock.hm(w.etaMs)}`,
      lines: [wind + along, `${RfFormat.format(w.temperatureC, 'degc')} · rain ${RfFormat.format(w.precipitationMmPerHour ?? 0, 'mmh')}${rh}`],
    };
  }

  /**
   * A weather tag above each route point; the return to the pilot shares the pilot's place, so it has none.
   * With a metric (phones) a tag holds the ETA and that value only.
   */
  private drawTags(map: MlMap | null, points: KeyPointWeatherVm[], metric: WeatherMetric | null): void {
    this.tags.forEach((m) => m.remove());
    this.tags = [];
    if (!map) return;
    const last = points.at(-1);
    const returns = points.length > 1 && !!last && this.samePlace(points[0], last);
    const shown = returns ? points.slice(0, -1) : points;
    for (const p of shown) {
      if (!p.reading) continue;
      const el = document.createElement('div');
      el.className = `rf-wx-tag is-${p.level}${metric ? ' rf-wx-tag--compact' : ''}`;
      el.textContent = metric ? `${p.eta} · ${RouteWeatherMapComponent.shortValue(p, metric)}` : this.tagText(p);
      el.setAttribute('aria-hidden', 'true');
      this.tags.push(this.placeTag(map, el, p));
    }
  }

  protected placeTag(map: MlMap, el: HTMLElement, p: KeyPointWeatherVm): Pick<Marker, 'remove'> {
    return new Marker({ element: el, anchor: 'bottom', offset: [0, -20] }).setLngLat([p.point.lon, p.point.lat]).addTo(map);
  }

  private tagText(p: KeyPointWeatherVm): string {
    const rain = p.rain === 'dry' ? '' : ` · ${p.rain}`;
    return `${p.eta} · ${p.temperature} · ${p.windSpeed} ${p.windCompass}${rain}`;
  }

  /** The value a phone tag shows for the metric the route is coloured by. */
  static shortValue(p: KeyPointWeatherVm, metric: WeatherMetric): string {
    switch (metric) {
      case 'headwind':
        return p.alongTrack || p.windSpeed;
      case 'wind':
        return `${p.windSpeed} ${p.windCompass}`.trim();
      case 'temperature':
        return p.temperature;
      default:
        return p.rain;
    }
  }

  private samePlace(a: KeyPointWeatherVm, b: KeyPointWeatherVm): boolean {
    return a.point.lat === b.point.lat && a.point.lon === b.point.lon;
  }
}
