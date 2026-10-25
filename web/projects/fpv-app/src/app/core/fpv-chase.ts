import { Injectable, computed, inject, signal } from '@angular/core';
import { LatLon, destinationPoint, initialBearingDeg } from '@rfmap/ui-kit';
import { AltitudeView, Corridor, CorridorState, FpvAltitudeWaypoint } from './fpv-altitude';

/** Ground speed the chase flies at when the module did not say: the climb assistant's default. */
export const CHASE_DEFAULT_SPEED_MPS = 15;
/** Playback rates: real time, and faster for a long route. */
export const CHASE_RATES = [1, 4, 16] as const;
export type ChaseRate = (typeof CHASE_RATES)[number];
/**
 * The camera sits this far behind the drone along its heading, this far to its left (so the line
 * ahead is seen at an angle, not end-on) and this far above it, m: steep enough that the ground
 * ahead fills the view and the map keeps its tilt under the 3D limit.
 */
const BEHIND_M = 300;
const SIDE_M = 60;
const ABOVE_M = 150;
/**
 * It looks at the ground this far ahead of the drone. The point is on the ground because the map
 * keeps its centre on the terrain: a target up in the air would be dropped onto it, camera and all.
 */
const LOOK_AHEAD_M = 500;
/** It never goes lower than this over the ground under it or under the drone, m. */
const CAMERA_CLEAR_M = 25;
/** The heading is the chord over this much route either side, so corners turn smoothly. */
const HEADING_SPAN_M = 150;
/** The vertical speed is the plan's slope over this much route either side. */
const SLOPE_SPAN_M = 50;
/** How far ahead the HUD looks for where the plan leaves the corridor, m. */
const AHEAD_M = 2000;

const BAD: ReadonlySet<CorridorState> = new Set<CorridorState>(['low', 'ground', 'high']);

/** What the chase shows at one point of the route. */
export interface ChaseFrame {
  s: number;
  timeS: number;
  lat: number;
  lon: number;
  mslM: number;
  groundMslM: number;
  aglM: number;
  /** The safe floor over the ground there, m */
  needAglM: number;
  state: CorridorState;
  leg: number;
  /** Direction of travel, degrees clockwise from north */
  headingDeg: number;
  /** The plan's climb (positive) or descent (negative) at the chase's ground speed, m/s */
  verticalMps: number;
}

/** The next thing ahead of the drone: a waypoint, or where the plan leaves the corridor. */
export interface ChaseAhead {
  kind: 'waypoint' | 'corridor';
  text: string;
  inM: number;
  tone: 'info' | 'warn';
}

/** One leg on the timeline, with the worst place of the plan on it. */
export interface ChaseLeg {
  leg: number;
  name: string;
  fromS: number;
  toS: number;
  tone: 'ok' | 'warn' | 'bad';
}

/** A camera placement: from where, at what height, looking at which point at what height (MSL × exaggeration). */
export interface ChaseCamera {
  from: LatLon;
  fromAltM: number;
  to: LatLon;
  toAltM: number;
}

/**
 * The planned flight as the chase camera flies it: at a steady ground speed along the corridor's
 * samples, at the plan's height. Pure, so the HUD and the camera read the same numbers.
 */
export class ChaseTrack {
  readonly length: number;
  readonly durationS: number;
  readonly legs: ChaseLeg[];

  constructor(
    readonly corridor: Corridor,
    private readonly waypoints: FpvAltitudeWaypoint[],
    readonly speedMps: number,
  ) {
    this.length = corridor.length;
    this.durationS = this.length / speedMps;
    this.legs = this.buildLegs();
  }

  clampS(s: number): number {
    return Math.min(this.length, Math.max(0, s));
  }

  sAt(timeS: number): number {
    return this.clampS(timeS * this.speedMps);
  }

  frame(s: number): ChaseFrame {
    const c = this.corridor;
    const at = this.clampS(s);
    const i = c.indexAt(at);
    const ground = c.valueAt('groundMslM', at);
    const msl = c.valueAt('plannedMslM', at);
    const a = this.clampS(at - SLOPE_SPAN_M);
    const b = this.clampS(at + SLOPE_SPAN_M);
    const slope = b > a ? (c.valueAt('plannedMslM', b) - c.valueAt('plannedMslM', a)) / (b - a) : 0;
    return {
      s: at,
      timeS: at / this.speedMps,
      lat: c.valueAt('lat', at),
      lon: c.valueAt('lon', at),
      mslM: msl,
      groundMslM: ground,
      aglM: msl - ground,
      needAglM: c.valueAt('floorMslM', at) - ground,
      state: c.state(i),
      leg: c.c.leg[i],
      headingDeg: this.headingAt(at),
      verticalMps: slope * this.speedMps,
    };
  }

  /** The next waypoint, and the next place within AHEAD_M where the plan leaves the corridor. */
  ahead(s: number): ChaseAhead[] {
    const out: ChaseAhead[] = [];
    const c = this.corridor;
    const here = c.state(c.indexAt(s));
    for (let i = c.indexAt(s) + 1; i < c.c.s.length && c.c.s[i] - s <= AHEAD_M; i++) {
      const st = c.state(i);
      if (BAD.has(st) && st !== here) {
        const need = Math.round(c.c.floorMslM[i] - c.c.groundMslM[i]);
        const text = st === 'high' ? 'Over the ceiling' : `Under the floor: needs ${need} m AGL`;
        out.push({ kind: 'corridor', text, inM: c.c.s[i] - s, tone: 'warn' });
        break;
      }
    }
    const next = this.waypoints.find((w) => w.s > s + 1);
    if (next)
      out.unshift({
        kind: 'waypoint',
        text: next.label ?? `WP${next.index}`,
        inM: next.s - s,
        tone: 'info',
      });
    else if (s < this.length - 1)
      out.unshift({
        kind: 'waypoint',
        text: this.pointName(this.legs.length),
        inM: this.length - s,
        tone: 'info',
      });
    return out;
  }

  /** Behind and above the drone, looking at the ground ahead of it; never into the ground. */
  camera(s: number, exaggeration: number): ChaseCamera {
    const f = this.frame(s);
    const behind = destinationPoint(f.lat, f.lon, (f.headingDeg + 180) % 360, BEHIND_M);
    const back = destinationPoint(behind.lat, behind.lon, (f.headingDeg + 270) % 360, SIDE_M);
    const groundBehind = this.corridor.valueAt('groundMslM', this.clampS(s - BEHIND_M));
    const low = Math.max(groundBehind, f.groundMslM) * exaggeration + CAMERA_CLEAR_M;
    const fromAltM = Math.max(f.mslM * exaggeration + ABOVE_M, low);
    const to = destinationPoint(f.lat, f.lon, f.headingDeg, LOOK_AHEAD_M);
    const toAltM =
      this.corridor.valueAt('groundMslM', this.clampS(s + LOOK_AHEAD_M)) * exaggeration;
    return { from: back, fromAltM, to, toAltM };
  }

  private headingAt(s: number): number {
    const c = this.corridor;
    const a = this.clampS(s - HEADING_SPAN_M);
    const b = this.clampS(s + HEADING_SPAN_M);
    const p = { lat: c.valueAt('lat', a), lon: c.valueAt('lon', a) };
    const q = { lat: c.valueAt('lat', b), lon: c.valueAt('lon', b) };
    return initialBearingDeg(p, q);
  }

  /** Point 0 is the pilot, and so is the last one of a route that returns. */
  private pointName(p: number): string {
    const w = this.waypoints.find((x) => x.index === p);
    if (p === 0 || !w) return 'Pilot';
    return w.label ?? `WP${p}`;
  }

  private buildLegs(): ChaseLeg[] {
    const c = this.corridor;
    const legs: ChaseLeg[] = [];
    for (let i = 0; i < c.c.s.length; i++) {
      const leg = c.c.leg[i];
      let cur = legs.at(-1);
      if (cur?.leg !== leg) {
        const fromS = cur?.toS ?? 0;
        cur = {
          leg,
          name: `${this.pointName(leg)} → ${this.pointName(leg + 1)}`,
          fromS,
          toS: c.c.s[i],
          tone: 'ok',
        };
        legs.push(cur);
      }
      cur.toS = c.c.s[i];
      const st = c.state(i);
      if (st === 'ground') cur.tone = 'bad';
      else if (BAD.has(st) && cur.tone === 'ok') cur.tone = 'warn';
    }
    return legs;
  }
}

/**
 * The chase camera over the 3D view: where along the route it is, whether it plays, and how fast.
 * It moves the profile cursor with it, so the 3D cursor is the drone and the profile follows.
 */
@Injectable({ providedIn: 'root' })
export class ChaseView {
  private readonly alt = inject(AltitudeView);

  readonly active = signal(false);
  readonly playing = signal(false);
  readonly rate = signal<ChaseRate>(4);
  /** Distance along the route, m */
  readonly s = signal(0);

  readonly track = computed(() => {
    const corridor = this.alt.corridor();
    if (!corridor || corridor.length <= 0) return null;
    const climb = this.alt.climb();
    const speed =
      climb?.airspeedMps && climb.airspeedMps > 0 ? climb.airspeedMps : CHASE_DEFAULT_SPEED_MPS;
    return new ChaseTrack(corridor, this.alt.result()?.waypoints ?? [], speed);
  });
  /** The drone's climb and descent rates, when the climb assistant ran. */
  readonly limits = computed(() => {
    const climb = this.alt.climb();
    return climb ? { climbMps: climb.climbRateMps, descentMps: climb.descentRateMps } : null;
  });
  readonly frame = computed(() => {
    const t = this.track();
    return t && this.active() ? t.frame(this.s()) : null;
  });
  readonly ahead = computed(() => {
    const t = this.track();
    return t && this.active() ? t.ahead(this.s()) : [];
  });

  /** Into 3D, from the profile cursor if there is one, and playing. */
  start(): void {
    const t = this.track();
    if (!t) return;
    this.alt.setMode('3d');
    const from = this.alt.cursorS();
    this.active.set(true);
    this.seek(from !== null && from < t.length - 1 ? from : 0);
    this.playing.set(true);
  }

  stop(): void {
    this.active.set(false);
    this.playing.set(false);
  }

  toggle(): void {
    const t = this.track();
    if (!t) return;
    // from the end, play again from the start
    if (!this.playing() && this.s() >= t.length - 1) this.seek(0);
    this.playing.update((p) => !p);
  }

  seek(s: number): void {
    const t = this.track();
    if (!t) return;
    const at = t.clampS(s);
    this.s.set(at);
    this.alt.cursorS.set(at);
  }

  /** Advances the playback by `dtS` of wall time; stops at the end. */
  tick(dtS: number): void {
    const t = this.track();
    if (!t || !this.playing() || !(dtS > 0)) return;
    const next = this.s() + dtS * t.speedMps * this.rate();
    this.seek(next);
    if (next >= t.length) this.playing.set(false);
  }
}
