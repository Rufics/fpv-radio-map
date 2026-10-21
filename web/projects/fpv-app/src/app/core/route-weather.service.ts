import { DestroyRef, Injectable, computed, effect, inject, signal, untracked } from '@angular/core';
import {
  ApiError,
  Mission,
  RfFormat,
  RfmapApiClient,
  RouteTimeline,
  RouteWeather,
  TimedPoint,
  TimelineStop,
  WeatherLevel,
  WeatherPoint,
  WeatherReading,
  WeatherSample,
  WeatherScale,
  Wind,
} from '@rfmap/ui-kit';
import { FpvWorkspace, pointName } from './fpv-workspace.service';

/** Speed used for arrival times when the mission names no drone from the library. */
export const FALLBACK_SPEED_MPS = 10;
/** Forecast samples between the route points: about one per kilometre, at most this many. */
const SAMPLE_STEP_M = 1000;
const MAX_SAMPLES = 24;
/** Forecast requests in flight at once. */
const PARALLEL = 4;
/** Wait after the last route edit before asking for forecasts. */
const DEBOUNCE_MS = 400;

export type RouteWeatherStatus = 'idle' | 'loading' | 'ready' | 'error';

/** A route point with the weather expected there at its arrival time. */
export interface KeyPointWeatherVm {
  name: string;
  alongM: number;
  eta: string;
  temperature: string;
  windSpeed: string;
  windCompass: string;
  /** Direction the wind arrow points (where the wind blows to), degrees */
  windArrowDeg: number | null;
  /** Wind along the leg that arrives at the point; empty at the start */
  alongTrack: string;
  rain: string;
  /** Wind and headwind on the arriving leg, the stronger of the two */
  windLevel: WeatherLevel;
  rainLevel: WeatherLevel;
  /** The point overall */
  level: WeatherLevel;
  reading: WeatherReading | null;
  point: TimedPoint;
}

/** One line of the route summary. */
export interface WeatherSummaryLine {
  key: string;
  text: string;
  level: WeatherLevel;
}

/** Which forecast the values come from. */
export interface WeatherSourceVm {
  model: string;
  run: string;
  tier: string;
  stale: boolean;
}

/** Hourly forecast at one place, for the briefing's wind chart. */
export interface HourlyWeather {
  timeMs: number;
  weather: WeatherPoint | null;
}

/** Formats times of the flight for display, in local time like the rest of the app. */
export class FlightClock {
  static hm(ms: number): string {
    return new Date(ms).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
  }

  static day(ms: number): string {
    return new Date(ms).toLocaleDateString('en-GB', { weekday: 'short', day: 'numeric', month: 'short' });
  }

  /** The present to the minute, so a route asked twice within a minute keeps its forecast key. */
  static now(): number {
    return Math.floor(Date.now() / 60e3) * 60e3;
  }
}

/**
 * Forecast requests with a cache. Places are rounded to 0.01° (about 1 km, finer than the forecast
 * grids) and times to the hour, because the server answers with the forecast hour nearest to the
 * time asked; nearby samples then share one request.
 */
export class ForecastCache {
  private readonly entries = new Map<string, Promise<WeatherPoint>>();

  constructor(private readonly fetch: (lat: number, lon: number, timeUtc: string) => Promise<WeatherPoint>) {}

  static hourOf(ms: number): number {
    return Math.round(ms / 3600e3) * 3600e3;
  }

  get(lat: number, lon: number, timeMs: number): Promise<WeatherPoint> {
    const la = Math.round(lat * 100) / 100;
    const lo = Math.round(lon * 100) / 100;
    const hour = ForecastCache.hourOf(timeMs);
    const key = `${la},${lo},${hour}`;
    let entry = this.entries.get(key);
    if (!entry) {
      entry = this.fetch(la, lo, new Date(hour).toISOString());
      // a failed request is asked again next time instead of being remembered
      entry.catch(() => this.entries.delete(key));
      this.entries.set(key, entry);
    }
    return entry;
  }

  clear(): void {
    this.entries.clear();
  }
}

/** Runs async jobs with at most `limit` of them in flight. */
export class LimitedRunner {
  constructor(private readonly limit: number) {}

  async run<T, R>(items: readonly T[], job: (item: T) => Promise<R>): Promise<PromiseSettledResult<R>[]> {
    const out: PromiseSettledResult<R>[] = [];
    for (let i = 0; i < items.length; i += this.limit) {
      out.push(...(await Promise.allSettled(items.slice(i, i + this.limit).map(job))));
    }
    return out;
  }
}

/** Builds the timeline of a mission: pilot, waypoints and the return, timed at the cruise speed. */
export class MissionTimelineBuilder {
  static stops(m: Mission): TimelineStop[] {
    const stops: TimelineStop[] = [{ name: pointName(m, -1), lat: m.pilot.lat, lon: m.pilot.lon }];
    m.waypoints.forEach((w, i) => stops.push({ name: pointName(m, i), lat: w.lat, lon: w.lon }));
    if (m.returnToPilot !== false && m.waypoints.length) stops.push({ name: pointName(m, -1), lat: m.pilot.lat, lon: m.pilot.lon });
    return stops;
  }

  /** True when the mission has no planned time and is flown now. */
  static flownNow(m: Mission): boolean {
    return !MissionTimelineBuilder.plannedUtc(m);
  }

  static departureMs(m: Mission, nowMs: number): number {
    const utc = MissionTimelineBuilder.plannedUtc(m);
    return utc ? new Date(utc).getTime() : nowMs;
  }

  private static plannedUtc(m: Mission): string | undefined {
    return m.time.mode === 'planned' ? m.time.utc : undefined;
  }

  /** Samples between the route points plus the points themselves, in route order. */
  static samplePoints(t: RouteTimeline): TimedPoint[] {
    const keys = t.keyPoints();
    const between = t.samples(SAMPLE_STEP_M, MAX_SAMPLES).filter((s) => !keys.some((k) => Math.abs(k.alongM - s.alongM) < 1));
    return [...keys, ...between].sort((a, b) => a.alongM - b.alongM);
  }
}

/**
 * Weather along the mission's route at the time the drone is expected at each place: the route
 * points and samples about every kilometre, from the server's /weather/point. Loaded in weather
 * mode only, for the side panel card, the briefing and the map; it does not change what the
 * calculation uses.
 */
@Injectable({ providedIn: 'root' })
export class RouteWeatherService {
  private readonly api = inject(RfmapApiClient);
  private readonly ws = inject(FpvWorkspace);
  private readonly cache = new ForecastCache((lat, lon, t) => this.api.weatherPoint(lat, lon, t));
  private readonly runner = new LimitedRunner(PARALLEL);
  private readonly nowMs = signal(FlightClock.now());
  private readonly loaded = signal<{ key: string; samples: WeatherSample[] } | null>(null);
  private timer: ReturnType<typeof setTimeout> | null = null;
  private generation = 0;

  readonly status = signal<RouteWeatherStatus>('idle');
  readonly error = signal<string | null>(null);

  readonly drone = computed(() => this.ws.data.drone(this.ws.mission()?.drone));
  readonly speedMps = computed(() => this.drone()?.cruiseAirspeedMps ?? FALLBACK_SPEED_MPS);
  /** Off when rf.weather is left out of the mission: the calculation then uses the standard atmosphere. */
  readonly usedInCalculation = computed(() => !!this.ws.mission()?.modules?.['rf.weather']);

  readonly timeline = computed<RouteTimeline | null>(() => {
    const m = this.ws.mission();
    if (!m || m.waypoints.length === 0) return null;
    return new RouteTimeline(MissionTimelineBuilder.stops(m), MissionTimelineBuilder.departureMs(m, this.nowMs()), this.speedMps());
  });

  /** What to load; nothing outside weather mode, so the plain workspace asks for no forecasts. */
  private readonly key = computed(() => {
    if (!this.ws.weatherMode()) return null;
    const t = this.timeline();
    return t ? JSON.stringify([t.stops, t.departureMs, t.speedMps]) : null;
  });

  readonly route = computed<RouteWeather | null>(() => {
    const t = this.timeline();
    const l = this.loaded();
    return t && l && l.key === this.key() ? new RouteWeather(t, l.samples) : null;
  });

  /** True once forecasts arrived but none covers the flight time. */
  readonly beyondForecast = computed(() => this.status() === 'ready' && !!this.route()?.empty);

  readonly keyPoints = computed<KeyPointWeatherVm[]>(() => {
    const t = this.timeline();
    const r = this.route();
    if (!t) return [];
    return t.keyPoints().map((p, i) => this.keyPointVm(p, i === 0, r?.readingAt(p.alongM) ?? null));
  });

  readonly summary = computed<WeatherSummaryLine[]>(() => {
    const r = this.route();
    const t = this.timeline();
    if (!r || !t || r.empty) return [];
    return [this.headwindLine(r, t), this.rainLine(r), this.temperatureLine(r)].filter((x): x is WeatherSummaryLine => !!x);
  });

  readonly source = computed<WeatherSourceVm | null>(() => {
    const w = this.route()?.nearestForecast(0);
    if (!w) return null;
    return {
      model: w.model ?? 'forecast',
      run: w.runUtc ? `${FlightClock.day(new Date(w.runUtc).getTime())} ${FlightClock.hm(new Date(w.runUtc).getTime())}` : '',
      tier: w.tier === 'long-range' ? 'long-range' : 'hourly',
      stale: !!w.stale,
    };
  });

  /** "15:00–15:27" for the flight, or empty without a route. */
  readonly window = computed(() => {
    const t = this.timeline();
    return t ? `${FlightClock.hm(t.departureMs)}–${FlightClock.hm(t.arrivalMs)}` : '';
  });

  constructor() {
    // a mission flown now starts now: move "now" on whenever such a mission changes
    effect(() => {
      const m = this.ws.mission();
      if (m && MissionTimelineBuilder.flownNow(m)) untracked(() => this.nowMs.set(FlightClock.now()));
    });
    effect(() => {
      const key = this.key();
      untracked(() => this.schedule(key));
    });
    inject(DestroyRef).onDestroy(() => this.cancel());
  }

  /** Asks again, with "now" moved to the present for a mission flown now. */
  refresh(): void {
    this.cache.clear();
    this.loaded.set(null);
    this.nowMs.set(FlightClock.now());
    this.schedule(this.key());
  }

  /** Hourly forecasts at a point from `hours` before to `hours` after its arrival time. */
  async hourly(point: TimedPoint, hours = 2): Promise<HourlyWeather[]> {
    const base = ForecastCache.hourOf(point.etaMs);
    const times = Array.from({ length: hours * 2 + 1 }, (_, i) => base + (i - hours) * 3600e3);
    const settled = await this.runner.run(times, (t) => this.cache.get(point.lat, point.lon, t));
    return settled.map((s, i) => ({ timeMs: times[i], weather: s.status === 'fulfilled' ? s.value : null }));
  }

  private schedule(key: string | null): void {
    this.cancel();
    if (!key) {
      this.status.set('idle');
      return;
    }
    this.status.set('loading');
    this.timer = setTimeout(() => void this.load(key), DEBOUNCE_MS);
  }

  private cancel(): void {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    // a load still in flight is for an older route: drop its answer
    this.generation++;
  }

  private async load(key: string): Promise<void> {
    const t = this.timeline();
    if (!t || key !== this.key()) return;
    const gen = this.generation;
    this.error.set(null);
    const points = MissionTimelineBuilder.samplePoints(t);
    const settled = await this.runner.run(points, (p) => this.cache.get(p.lat, p.lon, p.etaMs));
    if (gen !== this.generation) return;
    const failed = settled.find((s): s is PromiseRejectedResult => s.status === 'rejected');
    if (failed && settled.every((s) => s.status === 'rejected')) {
      this.status.set('error');
      this.error.set(ApiError.from(failed.reason).message);
      return;
    }
    const samples = points.map((point, i) => {
      const s = settled[i];
      return { point, weather: s.status === 'fulfilled' ? s.value : null };
    });
    this.loaded.set({ key, samples });
    this.status.set('ready');
  }

  private keyPointVm(p: TimedPoint, first: boolean, r: WeatherReading | null): KeyPointWeatherVm {
    const along = first ? undefined : r?.alongTrackMps;
    const windLevel = RouteWeatherService.worst([WeatherScale.level('wind', r?.windSpeedMps), WeatherScale.level('headwind', along)]);
    const rainLevel = WeatherScale.level('rain', r?.precipitationMmPerHour);
    return {
      name: p.name ?? '',
      alongM: p.alongM,
      eta: FlightClock.hm(p.etaMs),
      temperature: RfFormat.format(r?.temperatureC, 'degc'),
      windSpeed: RfFormat.format(r?.windSpeedMps, 'mps'),
      windCompass: r?.windFromDeg === undefined ? '' : Wind.compass(r.windFromDeg),
      windArrowDeg: r?.windFromDeg === undefined ? null : Wind.towardsDeg(r.windFromDeg),
      alongTrack: along === undefined ? '' : WeatherScale.format('headwind', along),
      rain: (r?.precipitationMmPerHour ?? 0) < 0.05 ? 'dry' : RfFormat.format(r?.precipitationMmPerHour, 'mmh'),
      windLevel,
      rainLevel,
      level: RouteWeatherService.worst([windLevel, rainLevel]),
      reading: r,
      point: p,
    };
  }

  private headwindLine(r: RouteWeather, t: RouteTimeline): WeatherSummaryLine {
    const head = r.strongestHeadwind();
    if (!head) return { key: 'wind', text: 'No headwind on any leg', level: 'calm' };
    const leg = `${t.stops[head.leg].name} → ${t.stops[head.leg + 1].name}`;
    return { key: 'wind', text: `Headwind up to ${RfFormat.format(-head.mps, 'mps')} on leg ${head.leg + 1} · ${leg}`, level: WeatherScale.level('headwind', head.mps) };
  }

  private rainLine(r: RouteWeather): WeatherSummaryLine {
    const first = r.firstRain();
    if (!first) return { key: 'rain', text: 'No rain expected on the route', level: 'calm' };
    const max = r.max('precipitationMmPerHour');
    return { key: 'rain', text: `Rain from ${FlightClock.hm(first.etaMs)}, up to ${RfFormat.format(max, 'mmh')}`, level: WeatherScale.level('rain', max ?? undefined) };
  }

  private temperatureLine(r: RouteWeather): WeatherSummaryLine | null {
    const lo = r.min('temperatureC');
    const hi = r.max('temperatureC');
    if (lo === null || hi === null) return null;
    const text = Math.round(lo) === Math.round(hi) ? `Temperature ${RfFormat.format(lo, 'degc')} along the flight` : `Temperature ${RfFormat.format(lo, 'degc')} to ${RfFormat.format(hi, 'degc')} along the flight`;
    const level = RouteWeatherService.worst([WeatherScale.level('temperature', lo), WeatherScale.level('temperature', hi)]);
    return { key: 'temperature', text, level };
  }

  private static worst(levels: WeatherLevel[]): WeatherLevel {
    if (levels.includes('strong')) return 'strong';
    return levels.includes('notable') ? 'notable' : 'calm';
  }
}
