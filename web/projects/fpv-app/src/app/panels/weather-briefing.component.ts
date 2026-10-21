import { Component, ElementRef, afterNextRender, computed, effect, inject, signal, untracked, viewChild } from '@angular/core';
import { RfFormat, WeatherLevel, WeatherPoint } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { FlightClock, HourlyWeather, KeyPointWeatherVm, RouteWeatherService } from '../core/route-weather.service';

/** A small line chart of wind speed over a few hours, in a fixed 180 × 40 box. */
export class WindSparkline {
  static readonly WIDTH = 180;
  static readonly HEIGHT = 40;

  constructor(private readonly hours: readonly HourlyWeather[]) {}

  /** SVG polyline points of the hours that have a wind speed. */
  points(): string {
    const v = this.values();
    // the y range is the hours' own range plus 1 m/s each side, so a change of a few m/s shows
    const lo = Math.max(0, Math.min(...v) - 1);
    const span = Math.max(...v) + 1 - lo;
    const step = WindSparkline.WIDTH / Math.max(1, this.hours.length - 1);
    return this.hours
      .map((h, i) => ({ x: i * step, v: h.weather?.windSpeedMps }))
      .filter((p): p is { x: number; v: number } => p.v !== undefined)
      .map((p) => `${p.x.toFixed(1)},${(WindSparkline.HEIGHT - 2 - ((p.v - lo) / span) * (WindSparkline.HEIGHT - 6)).toFixed(1)}`)
      .join(' ');
  }

  /** x of a time inside the chart, or null outside it. */
  xOf(timeMs: number): number | null {
    if (this.hours.length < 2) return null;
    const first = this.hours[0].timeMs;
    const last = this.hours.at(-1)!.timeMs;
    if (timeMs < first || timeMs > last) return null;
    return ((timeMs - first) / (last - first)) * WindSparkline.WIDTH;
  }

  label(): string {
    if (this.hours.length < 2) return '';
    const v = this.values();
    const peak = v.length ? `, peak ${RfFormat.format(Math.max(...v), 'mps')}` : '';
    return `Wind ${FlightClock.hm(this.hours[0].timeMs)}–${FlightClock.hm(this.hours.at(-1)!.timeMs)}${peak}`;
  }

  private values(): number[] {
    return this.hours.map((h) => h.weather?.windSpeedMps).filter((x): x is number => x !== undefined);
  }
}

interface BriefingCardVm {
  key: KeyPointWeatherVm;
  rows: { label: string; value: string }[];
  spark: { points: string; etaX: number | null; label: string } | null;
  /** The hours around the ETA have been asked for and answered */
  hoursLoaded: boolean;
}

/** What the briefing says while it has no forecast to judge by: never a calm verdict. */
const NO_VERDICT = {
  loading: { title: 'Loading the forecast…', tone: 'none' },
  error: { title: 'The forecast could not be loaded', tone: 'warn' },
  noForecast: { title: 'No forecast covers this flight time', tone: 'warn' },
};

const VERDICT: Record<WeatherLevel, { title: string; tone: string }> = {
  calm: { title: 'Calm weather on the route', tone: 'ok' },
  notable: { title: 'Flyable with caution', tone: 'warn' },
  strong: { title: 'Check the weather before flying', tone: 'blocked' },
};

/** Pre-flight weather briefing: a verdict, every route point at its arrival time, and the forecast source. */
@Component({
  selector: 'fpv-weather-briefing',
  host: { '(keydown.escape)': 'close()' },
  template: `
    <section #panel class="briefing" role="dialog" aria-modal="true" aria-labelledby="briefing-title" tabindex="-1">
      <div class="top">
        <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="close()">← Back to map</button>
        <button type="button" class="rf-btn rf-btn--sm rf-btn--muted" (click)="refresh()">Refresh forecast</button>
      </div>
      <div class="title">
        <h2 id="briefing-title">Weather briefing · {{ ws.mission()?.name }}</h2>
        <span class="rf-caption">{{ subtitle() }}</span>
      </div>
      <div class="verdict" [class]="'verdict tone-' + verdict().tone">
        <b>{{ verdict().title }}</b>
        <ul>
          @for (s of wx.summary(); track s.key) {
            <li>{{ s.text }}</li>
          }
        </ul>
      </div>
      <div class="cards">
        @for (c of cards(); track c.key.alongM) {
          <article class="pt" [class]="'pt lvl-' + c.key.level">
            <div class="pt-head"><b>{{ c.key.name }}</b><span class="rf-mono muted">ETA {{ c.key.eta }}</span></div>
            <div class="pt-now">
              <span class="temp rf-mono">{{ c.key.temperature }}</span>
              @if (c.key.windArrowDeg !== null) {
                <svg class="dial" viewBox="0 0 44 44" role="img" [attr.aria-label]="'Wind from ' + c.key.windCompass">
                  <circle cx="22" cy="22" r="20" />
                  <path d="M22 36V8M16 14L22 8L28 14" [attr.transform]="'rotate(' + c.key.windArrowDeg + ' 22 22)'" />
                </svg>
              }
            </div>
            <dl class="rf-mono">
              @for (r of c.rows; track r.label) {
                <dt>{{ r.label }}</dt><dd>{{ r.value }}</dd>
              }
            </dl>
            @if (c.spark; as s) {
              <figure>
                <svg viewBox="0 0 180 40" role="img" [attr.aria-label]="s.label">
                  <line class="base" x1="0" y1="39" x2="180" y2="39" />
                  <polyline [attr.points]="s.points" />
                  @if (s.etaX !== null) {<line class="eta" [attr.x1]="s.etaX" y1="2" [attr.x2]="s.etaX" y2="39" />}
                </svg>
                <figcaption class="rf-caption">{{ s.label }}</figcaption>
              </figure>
            } @else if (c.hoursLoaded) {
              <div class="rf-caption">No hourly wind around the ETA.</div>
            } @else {
              <div class="rf-caption">Loading the hours around the ETA…</div>
            }
          </article>
        }
      </div>
      @if (wx.source(); as src) {
        <div class="rf-caption source">
          Source: {{ src.model }}{{ src.run ? ' · run ' + src.run : '' }} · {{ src.tier }} forecast{{ src.stale ? ' · the run is old' : '' }}. Values are the forecast hour nearest to each arrival time; forecasts are model output, not measurements.
        </div>
      }
    </section>
  `,
  styles: `
    :host { position: absolute; inset: 0; z-index: 12; background: var(--rf-bg); overflow-y: auto; }
    .briefing { max-width: 1200px; margin: 0 auto; padding: 20px 24px 32px; display: flex; flex-direction: column; gap: 16px; outline: none; }
    .top { display: flex; justify-content: space-between; gap: 8px; }
    .title { display: flex; flex-direction: column; gap: 4px; }
    h2 { margin: 0; font-size: 22px; }
    .verdict { padding: 12px 14px; border-radius: var(--rf-radius-lg); border: 1px solid var(--rf-border); background: var(--rf-surface-1); display: flex; flex-direction: column; gap: 6px; }
    .verdict ul { margin: 0; padding-left: 18px; font-size: 14px; color: var(--rf-text-3); }
    .verdict.tone-ok { background: var(--rf-ok-bg); border-color: var(--rf-ok-border); } .verdict.tone-ok b { color: var(--rf-ok-text); }
    .verdict.tone-warn { background: var(--rf-warn-bg); border-color: var(--rf-warn-border); } .verdict.tone-warn b { color: var(--rf-warn-text); }
    .verdict.tone-blocked { background: var(--rf-blocked-bg); border-color: var(--rf-blocked-border); } .verdict.tone-blocked b { color: var(--rf-blocked-text); }
    .cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(210px, 1fr)); gap: 12px; }
    .pt { padding: 14px; border-radius: var(--rf-radius-lg); background: var(--rf-surface-1); border: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 10px; }
    .pt.lvl-notable { border-color: var(--rf-warn-border); }
    .pt.lvl-strong { border-color: var(--rf-blocked-border); }
    .pt-head { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; }
    .muted { color: var(--rf-text-2); font-size: 12px; }
    .pt-now { display: flex; align-items: center; justify-content: space-between; }
    .temp { font-size: 28px; font-weight: 500; }
    .dial { width: 44px; height: 44px; }
    .dial circle { fill: none; stroke: var(--rf-border); stroke-width: 2; }
    .dial path { fill: none; stroke: var(--rf-text); stroke-width: 2.2; stroke-linecap: round; }
    dl { margin: 0; display: grid; grid-template-columns: auto 1fr; gap: 4px 10px; font-size: 12px; }
    dt { color: var(--rf-text-2); }
    dd { margin: 0; text-align: right; }
    figure { margin: 0; display: flex; flex-direction: column; gap: 2px; }
    figure svg { width: 100%; height: 40px; }
    figure .base { stroke: var(--rf-border); }
    figure polyline { fill: none; stroke: var(--rf-video); stroke-width: 2; }
    figure .eta { stroke: var(--rf-text); stroke-dasharray: 2 2; }
    .source { border-top: 1px solid var(--rf-divider); padding-top: 12px; }
    :host-context(.phone) .briefing { padding: 16px; }
  `,
})
export class WeatherBriefingComponent {
  protected readonly ws = inject(FpvWorkspace);
  protected readonly wx = inject(RouteWeatherService);
  private readonly panel = viewChild<ElementRef<HTMLElement>>('panel');
  /** Hourly forecasts around each point's ETA, by place and time */
  private readonly hourly = signal<Map<string, HourlyWeather[]>>(new Map());
  /** Goes up on every refresh, so hours asked for before it are dropped. */
  private hoursGeneration = 0;

  protected readonly subtitle = computed(() => {
    const t = this.wx.timeline();
    if (!t) return '';
    const day = FlightClock.day(t.departureMs);
    return `${day} · ${this.wx.window()} · ${RfFormat.format(t.lengthM, 'km1')} · ${t.keyPoints().length} route points`;
  });

  protected readonly verdict = computed(() => {
    const status = this.wx.status();
    if (status === 'loading') return NO_VERDICT.loading;
    if (status === 'error') return NO_VERDICT.error;
    const route = this.wx.route();
    if (!route || route.empty) return NO_VERDICT.noForecast;
    const levels = new Set(this.wx.summary().map((s) => s.level));
    if (levels.has('strong')) return VERDICT.strong;
    return levels.has('notable') ? VERDICT.notable : VERDICT.calm;
  });

  protected readonly cards = computed<BriefingCardVm[]>(() => {
    const route = this.wx.route();
    const hourly = this.hourly();
    return this.wx.keyPoints().map((k) => {
      const hours = hourly.get(WeatherBriefingComponent.keyOf(k));
      return { key: k, rows: this.rows(k, route?.nearestForecast(k.alongM) ?? null), spark: hours ? this.spark(hours, k.point.etaMs) : null, hoursLoaded: !!hours };
    });
  });

  constructor() {
    afterNextRender(() => this.panel()?.nativeElement.focus());
    effect(() => {
      const points = this.wx.keyPoints();
      if (this.wx.status() !== 'ready') return;
      untracked(() => {
        for (const k of points) void this.loadHours(k);
      });
    });
  }

  /** Fresh forecasts for the route and for the hours around each ETA. */
  protected refresh(): void {
    this.hoursGeneration++;
    this.hourly.set(new Map());
    this.wx.refresh();
  }

  protected close(): void {
    this.ws.briefingOpen.set(false);
  }

  private async loadHours(k: KeyPointWeatherVm): Promise<void> {
    const key = WeatherBriefingComponent.keyOf(k);
    if (this.hourly().has(key)) return;
    const gen = this.hoursGeneration;
    const hours = await this.wx.hourly(k.point);
    // asked before a refresh: the fresh request is already on its way
    if (gen !== this.hoursGeneration) return;
    this.hourly.update((m) => new Map(m).set(key, hours));
  }

  private static keyOf(k: KeyPointWeatherVm): string {
    return `${k.point.lat},${k.point.lon},${k.point.etaMs}`;
  }

  private rows(k: KeyPointWeatherVm, w: WeatherPoint | null): { label: string; value: string }[] {
    const rows = [
      { label: 'Wind', value: `${k.windSpeed}${k.windCompass ? ' ' + k.windCompass : ''}` },
      { label: 'Along leg', value: k.alongTrack || '—' },
      { label: 'Rain', value: k.rain },
    ];
    if (!w) return rows;
    const extra = [
      { label: 'Humidity', value: w.relativeHumidity === undefined ? '—' : `${Math.round(w.relativeHumidity)}%` },
      { label: 'Pressure', value: RfFormat.format(w.pressureHpa, 'hpa') },
      { label: 'Air density', value: RfFormat.format(w.airDensityKgM3, 'kgm3') },
      { label: 'Refraction k', value: RfFormat.format(w.effectiveEarthFactor, 'n2') },
    ];
    return [...rows, ...extra];
  }

  private spark(hours: HourlyWeather[], etaMs: number): BriefingCardVm['spark'] {
    const s = new WindSparkline(hours);
    const points = s.points();
    return points ? { points, etaX: s.xOf(etaMs), label: s.label() } : null;
  }
}
