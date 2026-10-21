import { WeatherPoint } from '../models/api.models';
import { RouteTimeline, TimedPoint } from './route-timeline';
import { Wind } from './wind';

/** The forecast the server gave for one place and time on the route; null when it gave none. */
export interface WeatherSample {
  point: TimedPoint;
  weather: WeatherPoint | null;
}

/** Weather at one place on the route, at the time the drone is expected there. */
export interface WeatherReading {
  alongM: number;
  etaMs: number;
  lat: number;
  lon: number;
  leg: number;
  temperatureC?: number;
  windSpeedMps?: number;
  windFromDeg?: number;
  /** Wind along the track: positive tailwind, negative headwind */
  alongTrackMps?: number;
  precipitationMmPerHour?: number;
  relativeHumidity?: number;
}

/** A leg's strongest headwind. */
export interface LegHeadwind {
  leg: number;
  mps: number;
}

type NumericField = 'temperatureC' | 'windSpeedMps' | 'precipitationMmPerHour' | 'relativeHumidity';

const NUMERIC_FIELDS: readonly NumericField[] = ['temperatureC', 'windSpeedMps', 'precipitationMmPerHour', 'relativeHumidity'];

/**
 * Forecast samples along a route, read at any distance from the start by linear interpolation
 * between the nearest samples that hold a forecast. Samples beyond the forecast (tier "none") are
 * left out: there is no weather there to show, and none is made up.
 */
export class RouteWeather {
  private readonly covered: { point: TimedPoint; weather: WeatherPoint }[];

  constructor(
    private readonly timeline: RouteTimeline,
    samples: readonly WeatherSample[],
  ) {
    this.covered = samples
      .filter((s): s is { point: TimedPoint; weather: WeatherPoint } => RouteWeather.hasForecast(s.weather))
      .sort((a, b) => a.point.alongM - b.point.alongM);
  }

  /** True when the server answered with forecast values, not just a tier "none" stub. */
  static hasForecast(w: WeatherPoint | null | undefined): w is WeatherPoint {
    return !!w && w.tier !== 'none' && (w.temperatureC !== undefined || w.windSpeedMps !== undefined);
  }

  get empty(): boolean {
    return this.covered.length === 0;
  }

  /** The forecast of the sample nearest to `alongM`, for its model, run and tier. */
  nearestForecast(alongM: number): WeatherPoint | null {
    let best: WeatherPoint | null = null;
    let bestD = Infinity;
    for (const s of this.covered) {
      const d = Math.abs(s.point.alongM - alongM);
      if (d < bestD) {
        bestD = d;
        best = s.weather;
      }
    }
    return best;
  }

  /** Readings at every sample that holds a forecast, in route order. */
  readings(): WeatherReading[] {
    return this.covered.map((s) => this.reading(s.point, s.weather));
  }

  /** Weather at `alongM`, interpolated between the samples around it. */
  readingAt(alongM: number): WeatherReading | null {
    if (this.empty) return null;
    const point = this.timeline.pointAt(alongM);
    const after = this.covered.findIndex((s) => s.point.alongM >= point.alongM);
    // before the first forecast or past the last one, the nearest one holds
    if (after === 0) return this.reading(point, this.covered[0].weather);
    const last = this.covered.at(-1);
    if (after < 0 && last) return this.reading(point, last.weather);
    const a = this.covered[after - 1];
    const b = this.covered[after];
    const span = b.point.alongM - a.point.alongM;
    const t = span > 0 ? (point.alongM - a.point.alongM) / span : 0;
    return this.reading(point, RouteWeather.blend(a.weather, b.weather, t));
  }

  /** The leg with the strongest headwind, when any leg has one. */
  strongestHeadwind(): LegHeadwind | null {
    let worst: LegHeadwind | null = null;
    for (const r of this.readings()) {
      if (r.alongTrackMps === undefined || r.alongTrackMps >= 0) continue;
      if (!worst || r.alongTrackMps < worst.mps) worst = { leg: r.leg, mps: r.alongTrackMps };
    }
    return worst;
  }

  /** The first reading with at least `minMmPerHour` of rain, or null for a dry route. */
  firstRain(minMmPerHour = 0.1): WeatherReading | null {
    return this.readings().find((r) => (r.precipitationMmPerHour ?? 0) >= minMmPerHour) ?? null;
  }

  /** Largest value of a field along the route. */
  max(field: NumericField): number | null {
    const v = this.values(field);
    return v.length ? Math.max(...v) : null;
  }

  /** Smallest value of a field along the route. */
  min(field: NumericField): number | null {
    const v = this.values(field);
    return v.length ? Math.min(...v) : null;
  }

  private values(field: NumericField): number[] {
    return this.readings()
      .map((r) => r[field])
      .filter((x): x is number => x !== undefined);
  }

  private reading(point: TimedPoint, w: WeatherPoint): WeatherReading {
    const along =
      w.windSpeedMps !== undefined && w.windFromDeg !== undefined
        ? Wind.alongTrackMps(w.windSpeedMps, w.windFromDeg, this.timeline.trackOf(point.leg))
        : undefined;
    return {
      alongM: point.alongM,
      etaMs: point.etaMs,
      lat: point.lat,
      lon: point.lon,
      leg: point.leg,
      temperatureC: w.temperatureC,
      windSpeedMps: w.windSpeedMps,
      windFromDeg: w.windFromDeg,
      alongTrackMps: along,
      precipitationMmPerHour: w.precipitationMmPerHour,
      relativeHumidity: w.relativeHumidity,
    };
  }

  /** Values between two forecasts; a value only one of them has is taken from that one. */
  private static blend(a: WeatherPoint, b: WeatherPoint, t: number): WeatherPoint {
    const out: WeatherPoint = { ...(t < 0.5 ? a : b) };
    for (const f of NUMERIC_FIELDS) out[f] = RouteWeather.lerp(a[f], b[f], t);
    if (a.windFromDeg !== undefined && b.windFromDeg !== undefined) out.windFromDeg = Wind.lerpDeg(a.windFromDeg, b.windFromDeg, t);
    else out.windFromDeg = a.windFromDeg ?? b.windFromDeg;
    return out;
  }

  private static lerp(a: number | undefined, b: number | undefined, t: number): number | undefined {
    if (a === undefined) return b;
    if (b === undefined) return a;
    return a + (b - a) * t;
  }
}
