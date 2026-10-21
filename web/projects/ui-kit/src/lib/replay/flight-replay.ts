import { FlightBand, FlightChannel, FlightTimeline, FlightView } from '../models/flightlog.models';
import { LatLon } from '../tools/geo';

/** What the flown track is coloured by. */
export type TrackMetric = 'battery' | 'altitude' | 'speed' | 'link' | 'gps';

export interface TrackLegendItem {
  color: string;
  label: string;
}

/** One coloured piece of the flown track: consecutive samples in the same colour class. */
export interface TrackSegment {
  points: LatLon[];
  color: string;
}

/** Values of the flight at one instant; null where the log has nothing there. */
export interface ReplayReading {
  t: number;
  position: LatLon | null;
  values: Partial<Record<FlightChannel, number | null>>;
  band: FlightBand | null;
}

interface MetricSpec {
  title: string;
  channel: (tl: FlightTimeline) => FlightChannel;
  /** Upper bounds of the first classes; the last class has none */
  bounds: number[];
  colors: string[];
  labels: string[];
}

const NO_DATA = '#5B6878';

/**
 * Colour classes of the flown track. Low cell voltage and a weak link are red, like a blocked
 * leg in planning; the colours differ in lightness as well as hue.
 */
export class TrackScale {
  static readonly METRICS: readonly TrackMetric[] = ['battery', 'altitude', 'speed', 'link', 'gps'];

  private static readonly SPECS: Record<TrackMetric, MetricSpec> = {
    battery: {
      title: 'Cell voltage',
      channel: () => 'cellV',
      bounds: [3.3, 3.5, 3.8],
      colors: ['#FF6B5B', '#F5A524', '#F7E07B', '#35D0BA'],
      labels: ['< 3.30 V', '3.30–3.50 V', '3.50–3.80 V', '≥ 3.80 V'],
    },
    altitude: {
      title: 'Altitude',
      channel: () => 'altM',
      bounds: [20, 50, 100],
      colors: ['#B9CBFF', '#7BE6D6', '#F7BC55', '#FF8A7A'],
      labels: ['< 20 m', '20–50 m', '50–100 m', '≥ 100 m'],
    },
    speed: {
      title: 'Ground speed',
      channel: () => 'speedMps',
      bounds: [5, 15, 25],
      colors: ['#B9CBFF', '#7BE6D6', '#F7BC55', '#FF8A7A'],
      labels: ['< 18 km/h', '18–54 km/h', '54–90 km/h', '≥ 90 km/h'],
    },
    link: {
      title: 'Link',
      channel: (tl) => (tl.channels.linkQualityPct ? 'linkQualityPct' : 'rssiPct'),
      bounds: [30, 60],
      colors: ['#FF6B5B', '#F5A524', '#35D0BA'],
      labels: ['< 30 %', '30–60 %', '≥ 60 %'],
    },
    gps: {
      title: 'GPS satellites',
      channel: () => 'sats',
      bounds: [6, 10],
      colors: ['#FF6B5B', '#F5A524', '#35D0BA'],
      labels: ['< 6', '6–9', '≥ 10'],
    },
  };

  static title(metric: TrackMetric): string {
    return TrackScale.SPECS[metric].title;
  }

  static channel(metric: TrackMetric, timeline: FlightTimeline): FlightChannel {
    return TrackScale.SPECS[metric].channel(timeline);
  }

  /** The class of a value, or -1 without one. */
  static classOf(metric: TrackMetric, v: number | null | undefined): number {
    if (v === null || v === undefined || !Number.isFinite(v)) return -1;
    const bounds = TrackScale.SPECS[metric].bounds;
    const i = bounds.findIndex((b) => v < b);
    return i < 0 ? bounds.length : i;
  }

  static color(metric: TrackMetric, v: number | null | undefined): string {
    const c = TrackScale.classOf(metric, v);
    return c < 0 ? NO_DATA : TrackScale.SPECS[metric].colors[c];
  }

  static legend(metric: TrackMetric): TrackLegendItem[] {
    const s = TrackScale.SPECS[metric];
    return s.colors.map((color, i) => ({ color, label: s.labels[i] })).reverse();
  }

  /** Whether the flight logged what this metric colours by. */
  static available(metric: TrackMetric, timeline: FlightTimeline): boolean {
    const values = timeline.channels[TrackScale.channel(metric, timeline)];
    return !!values && values.some((v) => v !== null);
  }
}

/** The flight on its even time base: values and positions at any instant, and the track. */
export class ReplayTimeline {
  readonly start: number;
  readonly end: number;

  constructor(private readonly view: FlightView) {
    const t = view.timeline.t;
    this.start = t.length ? t[0] : 0;
    this.end = t.length ? t[t.length - 1] : 0;
  }

  get duration(): number {
    return this.end - this.start;
  }

  get hasTrack(): boolean {
    return (this.view.timeline.channels.lat ?? []).some((v) => v !== null);
  }

  /** The sample at or just before `t`. */
  index(t: number): number {
    const tl = this.view.timeline;
    if (!tl.t.length) return 0;
    const i = Math.floor((t - this.start) * tl.hz);
    return Math.min(tl.t.length - 1, Math.max(0, i));
  }

  /** A channel at `t`, linear between the two samples around it. */
  value(channel: FlightChannel, t: number): number | null {
    const values = this.view.timeline.channels[channel];
    if (!values?.length) return null;
    const i = this.index(t);
    const a = values[i];
    const b = values[Math.min(values.length - 1, i + 1)];
    if (a === null || a === undefined) return null;
    if (b === null || b === undefined) return a;
    const f = Math.min(1, Math.max(0, (t - this.view.timeline.t[i]) * this.view.timeline.hz));
    return a + (b - a) * f;
  }

  position(t: number): LatLon | null {
    const lat = this.value('lat', t);
    const lon = this.value('lon', t);
    return lat === null || lon === null ? null : { lat, lon };
  }

  band(t: number): FlightBand | null {
    const b = this.view.bands.find((x) => t >= x.from && t < x.to) ?? this.view.bands.at(-1);
    return b?.band ?? null;
  }

  reading(t: number): ReplayReading {
    const values: ReplayReading['values'] = {};
    for (const ch of Object.keys(this.view.timeline.channels) as FlightChannel[]) values[ch] = this.value(ch, t);
    return { t, position: this.position(t), values, band: this.band(t) };
  }

  /** Every sample with a position, for fitting the map. */
  positions(): LatLon[] {
    const { lat = [], lon = [] } = this.view.timeline.channels;
    const out: LatLon[] = [];
    for (let i = 0; i < lat.length; i++) {
      const a = lat[i];
      const b = lon[i];
      if (a !== null && b !== null && b !== undefined) out.push({ lat: a, lon: b });
    }
    return out;
  }

  /** The track split where the colour class changes or the position is missing. */
  segments(metric: TrackMetric): TrackSegment[] {
    const tl = this.view.timeline;
    const { lat = [], lon = [] } = tl.channels;
    const values = tl.channels[TrackScale.channel(metric, tl)] ?? [];
    const out: TrackSegment[] = [];
    let current: TrackSegment | null = null;
    let currentClass = -2;
    for (let i = 0; i < lat.length; i++) {
      const a = lat[i];
      const b = lon[i];
      if (a === null || b === null || b === undefined) {
        current = null;
        continue;
      }
      const p = { lat: a, lon: b };
      const c = TrackScale.classOf(metric, values[i]);
      if (current && c === currentClass) {
        current.points.push(p);
        continue;
      }
      // a new class starts where the last one ended, so the line has no holes
      const next: TrackSegment = { points: current ? [current.points[current.points.length - 1], p] : [p], color: TrackScale.color(metric, values[i]) };
      out.push(next);
      current = next;
      currentClass = c;
    }
    return out.filter((s) => s.points.length > 1);
  }
}

/** Display text of replay values. */
export class ReplayFormat {
  private static readonly BANDS: Record<FlightBand, { title: string; color: string }> = {
    DISARMED: { title: 'Disarmed', color: '#2A3542' },
    ACRO: { title: 'Acro', color: '#9DB4FF' },
    ANGLE: { title: 'Angle', color: '#35D0BA' },
    HORIZON: { title: 'Horizon', color: '#7BE6D6' },
    ALTHOLD: { title: 'Alt hold', color: '#B9CBFF' },
    POSHOLD: { title: 'Pos hold', color: '#B9CBFF' },
    GPS_RESCUE: { title: 'GPS Rescue', color: '#F5A524' },
    FAILSAFE: { title: 'Failsafe', color: '#FF6B5B' },
  };

  /** "07:42", or "1:02:03" past an hour. */
  static clock(seconds: number): string {
    const s = Math.max(0, Math.floor(seconds));
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const ss = String(s % 60).padStart(2, '0');
    return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${String(m).padStart(2, '0')}:${ss}`;
  }

  static band(band: FlightBand): string {
    return ReplayFormat.BANDS[band]?.title ?? band;
  }

  static bandColor(band: FlightBand): string {
    return ReplayFormat.BANDS[band]?.color ?? NO_DATA;
  }

  /** "2.31 km" from 1 km, "640 m" below. */
  static distance(m: number | null | undefined): string {
    if (m === null || m === undefined || !Number.isFinite(m)) return '–';
    return m >= 1000 ? `${(m / 1000).toFixed(2)} km` : `${Math.round(m)} m`;
  }

  static fixed(v: number | null | undefined, digits: number, unit: string): string {
    if (v === null || v === undefined || !Number.isFinite(v)) return '–';
    return `${v.toFixed(digits)}${unit ? ' ' + unit : ''}`;
  }

  static speed(mps: number | null | undefined): string {
    return mps === null || mps === undefined ? '–' : `${Math.round(mps * 3.6)} km/h`;
  }

  /** "Sun 2 Feb 2025, 15:15 UTC" from the log's start time; the log may have no clock. */
  static started(iso: string | undefined): string {
    if (!iso) return 'start time not logged';
    const d = new Date(iso);
    if (Number.isNaN(d.getTime())) return 'start time not logged';
    const day = d.toLocaleDateString('en-GB', { weekday: 'short', day: 'numeric', month: 'short', year: 'numeric', timeZone: 'UTC' });
    const time = d.toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', timeZone: 'UTC' });
    return `${day.replace(/,/g, '')}, ${time} UTC`;
  }
}
