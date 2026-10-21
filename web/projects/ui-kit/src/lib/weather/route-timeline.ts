import { LatLon, distanceM, initialBearingDeg, interpolate } from '../tools/geo';

/** A point the route passes through, in flight order (pilot, waypoints, pilot again on return). */
export interface TimelineStop extends LatLon {
  name: string;
}

/** A place on the route with the time the drone is expected there. */
export interface TimedPoint extends LatLon {
  /** Set for the route's own points (pilot, waypoints, return), not for samples between them. */
  name?: string;
  /** Distance flown from the start, metres */
  alongM: number;
  /** Expected time over the point, ms since the epoch */
  etaMs: number;
  /** 0-based leg the point lies on; a route point belongs to the leg that arrives at it. */
  leg: number;
  /** Direction the drone flies on that leg, degrees clockwise from north */
  trackDeg: number;
}

/**
 * When the drone is where along the route: distances along the legs and arrival times at a
 * constant speed from a departure time. Display only: the speed is the drone's cruise airspeed and
 * the wind is not applied.
 */
export class RouteTimeline {
  /** Distance from the start at each stop */
  private readonly cumulative: number[];
  private readonly tracks: number[];

  constructor(
    readonly stops: readonly TimelineStop[],
    readonly departureMs: number,
    readonly speedMps: number,
  ) {
    if (speedMps <= 0) throw new RangeError('speed must be positive');
    this.cumulative = [0];
    this.tracks = [];
    for (let i = 1; i < stops.length; i++) {
      this.cumulative.push(this.cumulative[i - 1] + distanceM(stops[i - 1], stops[i]));
      this.tracks.push(initialBearingDeg(stops[i - 1], stops[i]));
    }
  }

  get lengthM(): number {
    return this.cumulative.at(-1) ?? 0;
  }

  get legCount(): number {
    return Math.max(0, this.stops.length - 1);
  }

  get arrivalMs(): number {
    return this.etaAt(this.lengthM);
  }

  /** Expected time at `alongM` metres from the start. */
  etaAt(alongM: number): number {
    return this.departureMs + (this.clamp(alongM) / this.speedMps) * 1000;
  }

  /** Distance from the start at which leg `leg` begins. */
  legStartM(leg: number): number {
    return this.cumulative[Math.min(Math.max(leg, 0), this.cumulative.length - 1)];
  }

  /** The leg `alongM` lies on; a point exactly on a stop belongs to the leg arriving there. */
  legAt(alongM: number): number {
    const d = this.clamp(alongM);
    for (let i = 1; i < this.cumulative.length; i++) {
      if (d <= this.cumulative[i]) return i - 1;
    }
    return Math.max(0, this.legCount - 1);
  }

  /** Direction of flight on a leg. */
  trackOf(leg: number): number {
    return this.tracks[leg] ?? 0;
  }

  /** Position, time and leg at `alongM` metres from the start. */
  pointAt(alongM: number): TimedPoint {
    const d = this.clamp(alongM);
    if (this.legCount === 0) return { ...this.latLon(this.stops[0]), alongM: 0, etaMs: this.departureMs, leg: 0, trackDeg: 0 };
    const leg = this.legAt(d);
    const start = this.cumulative[leg];
    const len = this.cumulative[leg + 1] - start;
    const t = len > 0 ? (d - start) / len : 0;
    const pos = interpolate(this.stops[leg], this.stops[leg + 1], t);
    return { ...pos, alongM: d, etaMs: this.etaAt(d), leg, trackDeg: this.trackOf(leg) };
  }

  /** The route's own points with their arrival times. */
  keyPoints(): TimedPoint[] {
    return this.stops.map((s, i) => {
      const leg = Math.max(0, i - 1);
      const alongM = this.cumulative[i];
      return { ...this.latLon(s), name: s.name, alongM, etaMs: this.etaAt(alongM), leg, trackDeg: this.trackOf(leg) };
    });
  }

  /** Evenly spaced points from start to end, about `stepM` apart and at most `maxCount` of them. */
  samples(stepM: number, maxCount: number): TimedPoint[] {
    if (this.lengthM === 0) return [this.pointAt(0)];
    const count = Math.min(Math.max(2, Math.ceil(this.lengthM / stepM) + 1), Math.max(2, maxCount));
    const out: TimedPoint[] = [];
    for (let i = 0; i < count; i++) out.push(this.pointAt((this.lengthM * i) / (count - 1)));
    return out;
  }

  private clamp(alongM: number): number {
    return Math.min(Math.max(alongM, 0), this.lengthM);
  }

  private latLon(p: LatLon): LatLon {
    return { lat: p.lat, lon: p.lon };
  }
}
