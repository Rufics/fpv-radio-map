import { FlightModeBand, FlightStoryEvent, ReplayFormat } from '@rfmap/ui-kit';

/** Chart width in SVG units; the SVG stretches it to the drawer's width. */
export const CHART_W = 1000;

export interface ChartRowVm {
  key: string;
  label: string;
  /** SVG path of the values, gaps left open */
  path: string;
  /** Text of the highest and lowest value on the row */
  top: string;
  bottom: string;
  /** A horizontal guide (low cell, weak link), 0..1 from the top */
  guideY?: number;
  guideLabel?: string;
}

export interface BandRectVm {
  x: number;
  w: number;
  color: string;
  label: string;
}

export interface TickVm {
  x: number;
  severity: string;
  title: string;
}

/** Turns timeline values into SVG geometry for the replay drawer. */
export class ReplayChart {
  constructor(
    private readonly start: number,
    private readonly end: number,
  ) {}

  x(t: number): number {
    const span = this.end - this.start;
    return span > 0 ? ((t - this.start) / span) * CHART_W : 0;
  }

  /** The time at a fraction 0..1 of the chart width. */
  timeAt(fraction: number): number {
    return this.start + Math.min(1, Math.max(0, fraction)) * (this.end - this.start);
  }

  bands(bands: FlightModeBand[]): BandRectVm[] {
    return bands.map((b) => {
      const x = this.x(b.from);
      return { x, w: Math.max(0, this.x(b.to) - x), color: ReplayFormat.bandColor(b.band), label: ReplayFormat.band(b.band) };
    });
  }

  ticks(story: FlightStoryEvent[]): TickVm[] {
    return story.map((e) => ({ x: this.x(e.t), severity: e.severity, title: `${ReplayFormat.clock(e.t)} ${e.title}` }));
  }

  /**
   * The values as a path `height` tall, at most `maxPoints` vertices; null values break the line.
   * `range` fixes the value scale (a percentage), otherwise it fits the values.
   */
  path(t: number[], values: (number | null)[], height: number, range?: [number, number], maxPoints = 600): { d: string; min: number; max: number } {
    const step = Math.max(1, Math.ceil(values.length / maxPoints));
    const [lo, hi] = ReplayChart.extent(values);
    if (lo > hi) return { d: '', min: 0, max: 0 };
    const min = range ? range[0] : lo;
    const max = range ? range[1] : hi;
    const span = max - min || 1;
    let d = '';
    let pen = false;
    for (let i = 0; i < values.length; i += step) {
      const v = values[i];
      if (v === null || !Number.isFinite(v)) {
        pen = false;
        continue;
      }
      const y = height - ((v - min) / span) * height;
      d += `${pen ? 'L' : 'M'}${this.x(t[i]).toFixed(1)} ${y.toFixed(1)}`;
      pen = true;
    }
    return { d, min, max };
  }

  /** The lowest and highest value; lowest above highest when there is none. */
  static extent(values: (number | null)[]): [number, number] {
    let lo = Infinity;
    let hi = -Infinity;
    for (const v of values) {
      if (v === null || !Number.isFinite(v)) continue;
      lo = Math.min(lo, v);
      hi = Math.max(hi, v);
    }
    return [lo, hi];
  }
}
