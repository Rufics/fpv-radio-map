import { Fix, FixPreviewRow, Mission, MissionAltitude, Reason } from '@rfmap/ui-kit';
import type {
  FpvAltitudeCorridor,
  FpvAltitudeOptions,
  FpvClimbData,
  FpvClimbStats,
  FpvClimbStrategy,
  FpvClimbStrategyId,
} from '../core/fpv-altitude';
import { LIBRARY } from './reference-case';

const EPS = 1e-6;
/** Per sample and metre over the floor: the lower of two profiles with the same climb wins. */
const LOW_WEIGHT = 1e-6;
const RATE_SLACK = 0.05;
const MIN_SAVING_M = 10;
const MIN_SAVING_SHARE = 0.1;
/** A plan this far out of the corridor (re-sampled waypoints can shift the floor a little) is out. */
const CORRIDOR_TOLERANCE = 1;
const SAME_WH = 0.05;
const G = 9.80665;
/** An added waypoint whose removal adds no more climb than this, and keeps the line in the corridor, goes. */
const PRUNE_RISE_M = 0.5;
/** At most this many heights per sample: a finer grid is coarsened. */
const MAX_HEIGHTS = 400;

/** What the drone can do between two neighbouring samples; free in the take-off zone. */
interface Segment {
  maxRise: number;
  maxDrop: number;
  gs: number;
  climb: number;
  descent: number;
}

/** Time over a stretch: flying it, or climbing or descending its height where that takes longer. */
function stretchTime(seg: Segment, ds: number, rise: number): number {
  return Math.max(seg.gs > 0 ? ds / seg.gs : 0, rise / seg.climb, -rise / seg.descent);
}

interface Profile {
  id: FpvClimbStrategyId;
  mslM: number[];
  within: boolean;
}

/** The route the corridor was sampled along, as fpv-altitude-mock builds it. */
export interface ClimbRoute {
  /** Pilot, the waypoints, and the pilot again on a return; h in the mission's reference */
  pts: { lat: number; lon: number; h: number; label?: string }[];
  cum: number[];
  returns: boolean;
  /** MSL for a height in the mission's reference over the ground */
  msl: (h: number, ground: number) => number;
  launch: number;
}

const fixed = (v: number, d: number) => v.toFixed(d);
const km = (m: number) => `km ${fixed(m / 1000, 2)}`;
const signedM = (m: number) => `${m >= 0 ? '+' : ''}${fixed(m, 0)} m`;
function minutes(s: number): string {
  const whole = Math.round(s);
  return `${Math.floor(whole / 60)}:${String(whole % 60).padStart(2, '0')}`;
}

/** Mock of ClimbPlanner in modules/fpv/altitude/src/climb_planner.cpp. */
export class ClimbPlannerMock {
  private readonly start: number;
  private readonly end: number;
  /** The grid step used: the one asked for, coarsened to keep at most 400 heights per sample. */
  readonly step: number;

  constructor(
    private readonly c: FpvAltitudeCorridor,
    private readonly near: boolean[],
    private readonly seg: Segment[],
    private readonly returns: boolean,
    heightStepM: number,
  ) {
    this.start = c.plannedMslM[0];
    this.end = c.plannedMslM.at(-1) ?? this.start;
    const span = Math.max(0, ...c.s.map((_, i) => c.ceilingMslM[i] - c.floorMslM[i]));
    this.step = Math.max(heightStepM, span / MAX_HEIGHTS);
  }

  private floor(i: number): number {
    return this.c.floorMslM[i];
  }

  highestFloor(): number {
    let top = this.start;
    const last = this.returns ? this.c.s.length - 1 : this.c.s.length;
    for (let i = 1; i < last; i++) top = Math.max(top, this.floor(i));
    return top;
  }

  /** The heights a sample may take: on the grid through the highest floor, between floor and ceiling. */
  private candidates(): number[][] {
    const n = this.c.s.length;
    let lo = Math.min(this.start, this.end);
    let hi = Math.max(this.start, this.end);
    for (let i = 0; i < n; i++) {
      lo = Math.min(lo, this.floor(i));
      hi = Math.max(hi, this.c.ceilingMslM[i], this.floor(i));
    }
    const top = this.highestFloor();
    const base = top - Math.ceil((top - lo) / this.step - EPS) * this.step;
    const levels = Math.ceil((hi - base) / this.step) + 1;
    return this.c.s.map((_, i) => {
      if (i === 0) return [this.start];
      if (this.returns && i === n - 1) return [this.end];
      const floor = this.floor(i);
      const cap = Math.max(this.c.ceilingMslM[i], floor);
      const out: number[] = [];
      for (let k = 0; k < levels; k++) {
        const v = base + k * this.step;
        if (v >= floor - EPS && v <= cap + EPS) out.push(v);
      }
      return out.length ? out : [floor];
    });
  }

  /** Least total climb through the corridor; null when the rates leave no way through. */
  search(rates: boolean): number[] | null {
    const h = this.candidates();
    const cost: number[][] = [[0]];
    const from: number[][] = [[0]];
    for (let i = 0; i + 1 < h.length; i++) {
      const s = this.seg[i];
      const floor = this.floor(i + 1);
      cost.push(h[i + 1].map(() => Infinity));
      from.push(h[i + 1].map(() => 0));
      h[i + 1].forEach((next, j) => {
        const low = LOW_WEIGHT * Math.max(0, next - floor);
        h[i].forEach((here, k) => {
          const d = next - here;
          const ok = !rates || (d <= s.maxRise + EPS && -d <= s.maxDrop + EPS);
          const v = cost[i][k] + Math.max(0, d) + low;
          if (ok && v < cost[i + 1][j]) {
            cost[i + 1][j] = v;
            from[i + 1][j] = k;
          }
        });
      });
    }
    const lastCost = cost.at(-1)!;
    let k = lastCost.indexOf(Math.min(...lastCost));
    if (!Number.isFinite(lastCost[k])) return null;
    const out = new Array<number>(h.length);
    for (let i = h.length - 1; i >= 0; i--) {
      out[i] = h[i][k];
      k = from[i][k];
    }
    return out;
  }

  withinRates(p: number[]): boolean {
    return p.every(
      (v, i) =>
        i + 1 >= p.length ||
        (p[i + 1] - v <= this.seg[i].maxRise + EPS && v - p[i + 1] <= this.seg[i].maxDrop + EPS),
    );
  }

  energyOptimal(): Profile {
    const p = this.search(true);
    if (p) return { id: 'energy', mslM: p, within: true };
    const any = this.search(false) ?? this.c.s.map(() => this.highestFloor());
    return { id: 'energy', mslM: any, within: false };
  }

  overLaunch(): Profile {
    const out = this.c.s.map(() => this.highestFloor());
    out[0] = this.start;
    if (this.returns) out[out.length - 1] = this.end;
    return { id: 'over-launch', mslM: out, within: this.withinRates(out) };
  }

  /** The last sample of the climb to the highest floor, and the first of the descent from it. */
  private peak(first: number, last: number, top: number): [number, number] {
    let peak = first;
    let peakEnd = first;
    for (let i = first; i <= last; i++) {
      if (this.floor(i) < top - EPS) continue;
      if (peakEnd === first && this.floor(peak) < top - EPS) peak = i;
      peakEnd = i;
    }
    if (this.floor(peak) < top - EPS) peak = peakEnd;
    return [peak, peakEnd];
  }

  gradual(): Profile {
    const n = this.c.s.length;
    const s = this.c.s;
    let first = 1;
    while (first < n && this.near[first]) first++;
    let last = this.returns ? n - 2 : n - 1;
    while (last > first && this.near[last]) last--;
    if (first >= n || first > last) return { ...this.overLaunch(), id: 'gradual' };
    const top = this.highestFloor();
    const [peak, peakEnd] = this.peak(first, last, top);
    const base = Math.max(this.start, this.floor(first));
    let up = 0;
    for (let i = first + 1; i <= peak; i++)
      up = Math.max(up, (this.floor(i) - base) / (s[i] - s[first]));
    const land = Math.max(this.end, this.floor(last));
    let down = 0;
    for (let i = peakEnd; this.returns && i < last; i++)
      down = Math.max(down, (this.floor(i) - land) / (s[last] - s[i]));
    const out = s.map((si, i) => {
      if (i < first) return Math.max(base, this.floor(i));
      if (i > last) return Math.max(land, this.floor(i));
      const v = Math.min(top, base + up * (si - s[first]));
      return this.returns ? Math.min(v, land + down * (s[last] - si)) : v;
    });
    out[0] = this.start;
    if (this.returns) out[n - 1] = this.end;
    return { id: 'gradual', mslM: out, within: this.withinRates(out) };
  }

  stats(p: number[]): Omit<FpvClimbStats, 'waypoints'> {
    const st = {
      riseM: 0,
      timeS: 0,
      minClearanceM: Infinity,
      minClearanceAtM: 0,
      overCeilingM: 0,
      steepestClimbMps: 0,
      steepestClimbAtM: 0,
    };
    p.forEach((v, i) => {
      const s = this.c.s[i];
      if (!this.near[i] && v - this.floor(i) < st.minClearanceM) {
        st.minClearanceM = v - this.floor(i);
        st.minClearanceAtM = s;
      }
      // Where the floor is over the ceiling, every profile has to be over it.
      if (this.c.ceilingMslM[i] >= this.floor(i))
        st.overCeilingM = Math.max(st.overCeilingM, v - this.c.ceilingMslM[i]);
      if (i + 1 === p.length) return;
      const d = p[i + 1] - v;
      const ds = this.c.s[i + 1] - s;
      const seg = this.seg[i];
      st.riseM += Math.max(0, d);
      st.timeS += stretchTime(seg, ds, d);
      const rate = (d / ds) * seg.gs;
      if (ds > 0 && Number.isFinite(seg.maxRise) && rate > st.steepestClimbMps) {
        st.steepestClimbMps = rate;
        st.steepestClimbAtM = s;
      }
    });
    if (!Number.isFinite(st.minClearanceM)) st.minClearanceM = 0;
    return st;
  }
}

/** Mock of ProfileFitter in modules/fpv/altitude/src/profile_fit.cpp. */
export class ProfileFitterMock {
  constructor(
    private readonly c: FpvAltitudeCorridor,
    private readonly route: ClimbRoute,
    private readonly m: Mission,
    private readonly tolerance: number,
  ) {}

  private sampleAt(s: number): number {
    let best = 0;
    this.c.s.forEach((v, i) => {
      if (Math.abs(v - s) < Math.abs(this.c.s[best] - s)) best = i;
    });
    return best;
  }

  /** Height in the mission's reference for an MSL height at sample i, rounded up to 0.1 m. */
  private reference(i: number, msl: number): number {
    const zero = this.route.msl(0, this.c.groundMslM[i]);
    return Math.ceil((msl - zero) * 10 - EPS) / 10;
  }

  private lineAt(i: number, a: number, ra: number, b: number, rb: number): number {
    const span = this.c.s[b] - this.c.s[a];
    const t = span > 0 ? (this.c.s[i] - this.c.s[a]) / span : 0;
    return this.route.msl(ra + (rb - ra) * t, this.c.groundMslM[i]);
  }

  /** Whether a line at sample i leaves the corridor where the profile is inside it. */
  private outside(i: number, line: number, profile: number): boolean {
    const floor = this.c.floorMslM[i];
    const ceiling = this.c.ceilingMslM[i];
    const under = profile >= floor - EPS && line < floor - EPS;
    // Where the floor is over the ceiling there is no corridor to keep under.
    const over = ceiling >= floor && profile <= ceiling + EPS && line > ceiling + EPS;
    return under || over;
  }

  /** Total climb along the lines between knots[from] and knots[to]. */
  private rise(ref: number[], knots: number[], from: number, to: number): number {
    let total = 0;
    for (let j = from; j < to; j++) {
      const [a, b] = [knots[j], knots[j + 1]];
      for (let i = a + 1; i <= b; i++)
        total += Math.max(
          0,
          this.lineAt(i, a, ref[a], b, ref[b]) - this.lineAt(i - 1, a, ref[a], b, ref[b]),
        );
    }
    return total;
  }

  /** Drops the knots (ends kept) whose line without them stays in the corridor and climbs no more. */
  private prune(profile: number[], ref: number[], knots: number[]): void {
    let j = 1;
    while (j + 1 < knots.length) {
      const [a, b] = [knots[j - 1], knots[j + 1]];
      let keep = this.rise(ref, [a, b], 0, 1) > this.rise(ref, knots, j - 1, j + 1) + PRUNE_RISE_M;
      for (let i = a + 1; !keep && i < b; i++)
        keep = this.outside(i, this.lineAt(i, a, ref[a], b, ref[b]), profile[i]);
      if (keep) j++;
      else knots.splice(j, 1);
    }
  }

  private split(profile: number[], ref: number[], a: number, b: number, knots: number[]): void {
    if (b <= a + 1) return;
    let worst = a;
    let score = this.tolerance;
    for (let i = a + 1; i < b; i++) {
      const line = this.lineAt(i, a, ref[a], b, ref[b]);
      const sc = Math.abs(line - profile[i]) + (this.outside(i, line, profile[i]) ? 1e6 : 0);
      if (sc > score) {
        worst = i;
        score = sc;
      }
    }
    if (worst === a) return;
    ref[worst] = this.reference(worst, profile[worst]);
    knots.push(worst);
    this.split(profile, ref, a, worst, knots);
    this.split(profile, ref, worst, b, knots);
  }

  fit(profile: number[]): { waypoints: Record<string, unknown>[]; added: number; mslM: number[] } {
    const field = MissionAltitude.field(this.m);
    const ref = this.c.s.map(() => 0);
    const mslM = this.c.s.map(() => 0);
    const waypoints: Record<string, unknown>[] = [];
    let added = 0;
    const { pts, cum } = this.route;
    for (let leg = 0; leg + 1 < pts.length; leg++) {
      const isReturn = this.route.returns && leg === pts.length - 2;
      const a = this.sampleAt(cum[leg]);
      const b = this.sampleAt(cum[leg + 1]);
      if (leg === 0) ref[a] = pts[0].h;
      ref[b] = isReturn ? pts[leg + 1].h : this.reference(b, profile[b]);
      const inner: number[] = [];
      this.split(profile, ref, a, b, inner);
      const knots = [a, ...inner.sort((x, y) => x - y), b];
      this.prune(profile, ref, knots);
      for (const k of knots.slice(1, -1)) {
        added++;
        waypoints.push({
          lat: this.c.lat[k],
          lon: this.c.lon[k],
          label: `C${added}`,
          [field]: ref[k],
          heightSource: 'manual',
        });
      }
      if (!isReturn)
        waypoints.push({ ...this.m.waypoints[leg], [field]: ref[b], heightSource: 'manual' });
      for (let j = 0; j + 1 < knots.length; j++)
        for (let i = knots[j]; i <= knots[j + 1]; i++)
          mslM[i] = this.lineAt(i, knots[j], ref[knots[j]], knots[j + 1], ref[knots[j + 1]]);
    }
    return { waypoints, added, mslM };
  }
}

/** The drone as the assistant uses it; energy only with a drone from the library (no wind in the mock). */
function performance(m: Mission, o: FpvAltitudeOptions, route: ClimbRoute) {
  const drone = LIBRARY.drones.find((d) => d.id === m.drone);
  const notes: string[] = [];
  let climb = 0;
  let airspeed = 15;
  let wh: ((rise: number) => number) | null = null;
  if (drone) {
    // The speed option flies the drone at that airspeed: ground speed, time and energy alike.
    airspeed = (o.speedMps ?? 0) > 0 ? o.speedMps! : drone.cruiseAirspeedMps;
    if (drone.climb.method === 'measured') climb = drone.climb.climbRateMps ?? 0;
    const cruiseWh = ((route.cum.at(-1) ?? 0) / airspeed) * (drone.cruisePowerW / 3600);
    const perM =
      drone.climb.method === 'measured' && drone.climb.climbPowerW && drone.climb.climbRateMps
        ? drone.climb.climbPowerW / drone.climb.climbRateMps / 3600
        : (drone.massKg * G) / (drone.climb.efficiency ?? 0.6) / 3600;
    wh = (rise) => cruiseWh + perM * rise;
    notes.push(
      `Energy: cruise at ${fixed(airspeed, 1)} m/s airspeed in the forecast wind, plus climbing; descents give nothing back.`,
    );
  } else {
    notes.push(
      `No drone picked: energy is not estimated, and the profiles assume ${fixed(airspeed, 0)} m/s.`,
    );
  }
  if ((o.speedMps ?? 0) > 0) airspeed = o.speedMps!;
  if ((o.climbRateMps ?? 0) > 0) climb = o.climbRateMps!;
  else if (climb <= 0) {
    climb = 3;
    notes.push(
      `Climb rate ${fixed(climb, 1)} m/s assumed: set climbRateMps, or measure the drone's climb in the library.`,
    );
  }
  return { drone, climb, descent: o.descentRateMps ?? 4, airspeed, wh, notes };
}

type Perf = ReturnType<typeof performance>;

function statsOf(
  st: Omit<FpvClimbStats, 'waypoints'>,
  perf: Perf,
  waypoints: number,
): FpvClimbStats {
  return { ...st, waypoints, ...(perf.wh ? { energyWh: perf.wh(st.riseM) } : {}) };
}

function previewRows(now: FpvClimbStats, after: FpvClimbStats): FixPreviewRow[] {
  const rows: FixPreviewRow[] = [
    { label: 'Total climb', now: `${fixed(now.riseM, 0)} m`, after: `${fixed(after.riseM, 0)} m` },
  ];
  if (now.energyWh !== undefined && after.energyWh !== undefined)
    rows.push({
      label: 'Route energy',
      now: `${fixed(now.energyWh, 1)} Wh`,
      after: `${fixed(after.energyWh, 1)} Wh`,
    });
  rows.push(
    { label: 'Flight time', now: minutes(now.timeS), after: minutes(after.timeS) },
    {
      label: 'Lowest height over the safe floor',
      now: signedM(now.minClearanceM),
      after: signedM(after.minClearanceM),
    },
    {
      label: 'Steepest climb',
      now: `${fixed(now.steepestClimbMps, 1)} m/s`,
      after: `${fixed(after.steepestClimbMps, 1)} m/s`,
    },
    { label: 'Waypoints', now: String(now.waypoints), after: String(after.waypoints) },
  );
  return rows;
}

function strategyText(
  p: Profile,
  perf: Perf,
  route: ClimbRoute,
): { title: string; summary: string } {
  const top = `${fixed(Math.max(...p.mslM) - route.launch, 0)} m`;
  const rate = `${fixed(perf.climb, 1)} m/s`;
  if (p.id === 'energy')
    return {
      title: 'Energy-optimal climb',
      summary: p.within
        ? `The least total climb through the corridor, climbing at most ${rate} past the take-off zone.`
        : `No profile through the corridor keeps to the drone's ${rate} climb; this one has the least total climb.`,
    };
  if (p.id === 'over-launch')
    return {
      title: 'Climb over launch',
      summary: `Climb to ${top} above launch over the pilot, then fly level.`,
    };
  return {
    title: 'Gradual up and down',
    summary: route.returns
      ? `Climb steadily to ${top} above launch and descend steadily back to the pilot.`
      : `Climb steadily to ${top} above launch, then fly level.`,
  };
}

/** The advice to fly the energy-optimal profile: it keeps the route in the corridor, or climbs less. */
function adviceText(now: FpvClimbStats, energy: FpvClimbStats, intoCorridor: boolean): string {
  let wh = '';
  if (now.energyWh !== undefined && energy.energyWh !== undefined) {
    const d = now.energyWh - energy.energyWh;
    if (Math.abs(d) >= SAME_WH) wh = ` (${fixed(Math.abs(d), 1)} Wh ${d > 0 ? 'less' : 'more'})`;
  }
  const climbs = `${fixed(energy.riseM, 0)} m in all`;
  return intoCorridor
    ? `Climb assistant: the energy-optimal profile keeps the whole route inside the corridor, climbing ${climbs} against the plan's ${fixed(now.riseM, 0)} m${wh}.`
    : `Climb assistant: the energy-optimal profile climbs ${climbs} instead of ${fixed(now.riseM, 0)} m${wh}.`;
}

/** Legs where the plan climbs faster than the drone can, past the take-off zone. */
function steepLegs(
  c: FpvAltitudeCorridor,
  seg: Segment[],
  route: ClimbRoute,
  perf: Perf,
  fix: Fix,
): Reason[] {
  const worst = new Map<number, { rate: number; at: number; gs: number }>();
  for (let i = 0; i + 1 < c.s.length; i++) {
    const ds = c.s[i + 1] - c.s[i];
    if (!Number.isFinite(seg[i].maxRise) || ds <= 0) continue;
    const rate = ((c.plannedMslM[i + 1] - c.plannedMslM[i]) / ds) * seg[i].gs;
    const leg = c.leg[i + 1];
    if (rate > perf.climb + RATE_SLACK && rate > (worst.get(leg)?.rate ?? -Infinity))
      worst.set(leg, { rate, at: c.s[i], gs: seg[i].gs });
  }
  // The pilot starts the route, and ends it only on a return.
  const name = (p: number) =>
    p === 0 || (route.returns && p === route.pts.length - 1)
      ? 'Pilot'
      : (route.pts[p].label ?? `WP${p}`);
  return [...worst].map(([leg, w]) => ({
    module: 'fpv.altitude',
    severity: 'warn' as const,
    leg,
    text: `Leg ${name(leg)} → ${name(leg + 1)}: the plan climbs at ${fixed(w.rate, 1)} m/s at ${km(w.at)} (${fixed(w.gs, 1)} m/s over the ground); the drone climbs ${fixed(perf.climb, 1)} m/s.`,
    fix,
  }));
}

/** Mock of climbAssist in modules/fpv/altitude/src/climb_assistant.cpp. */
export function fpvClimbMock(
  m: Mission,
  o: FpvAltitudeOptions,
  c: FpvAltitudeCorridor,
  near: boolean[],
  route: ClimbRoute,
): { data: FpvClimbData; reasons: Reason[] } {
  const perf = performance(m, o, route);
  const seg: Segment[] = c.s.slice(1).map((s, i) => {
    const ds = s - c.s[i];
    const free = near[i] || near[i + 1];
    const gs = perf.airspeed;
    return {
      maxRise: free ? Infinity : (perf.climb * ds) / gs,
      maxDrop: free ? Infinity : (perf.descent * ds) / gs,
      gs,
      climb: perf.climb,
      descent: perf.descent,
    };
  });
  const step = o.heightStepM ?? 5;
  const planner = new ClimbPlannerMock(c, near, seg, route.returns, step);
  const fitter = new ProfileFitterMock(c, route, m, o.fitToleranceM ?? 5);
  const now = statsOf(planner.stats(c.plannedMslM), perf, m.waypoints.length);
  const profiles = [planner.energyOptimal(), planner.overLaunch(), planner.gradual()];
  const strategies: FpvClimbStrategy[] = profiles.map((p) => {
    const fit = fitter.fit(p.mslM);
    const after = statsOf(planner.stats(fit.mslM), perf, fit.waypoints.length);
    const { title, summary } = strategyText(p, perf, route);
    const fix: Fix = {
      id: `fpv.altitude/climb-${p.id}`,
      title: `Fly the ${title[0].toLowerCase()}${title.slice(1)}`,
      patch: { waypoints: fit.waypoints },
      preview: previewRows(now, after),
    };
    return {
      ...after,
      id: p.id,
      title,
      summary,
      withinClimbRate: p.within && after.steepestClimbMps <= perf.climb + RATE_SLACK,
      addedWaypoints: fit.added,
      mslM: fit.mslM.map((v) => Math.round(v * 10) / 10),
      fix,
    };
  });
  const energy = strategies[0];
  const reasons = steepLegs(c, seg, route, perf, energy.fix);
  const outside = now.minClearanceM < -CORRIDOR_TOLERANCE || now.overCeilingM > CORRIDOR_TOLERANCE;
  const inside =
    energy.minClearanceM >= -CORRIDOR_TOLERANCE && energy.overCeilingM <= CORRIDOR_TOLERANCE;
  const saving = now.riseM - energy.riseM;
  if ((outside && inside) || saving >= Math.max(MIN_SAVING_M, MIN_SAVING_SHARE * now.riseM)) {
    reasons.push({
      module: 'fpv.altitude',
      severity: 'info',
      text: adviceText(now, energy, outside && inside),
      fix: energy.fix,
    });
  }
  const data: FpvClimbData = {
    recommended: 'energy',
    climbRateMps: perf.climb,
    descentRateMps: perf.descent,
    airspeedMps: perf.airspeed,
    energyEstimated: !!perf.wh,
    heightStepM: planner.step,
    fitToleranceM: o.fitToleranceM ?? 5,
    plan: now,
    strategies,
    notes: [
      ...perf.notes,
      `Past the take-off zone the profiles climb at most ${fixed(perf.climb, 1)} m/s and descend at most ${fixed(perf.descent, 1)} m/s at the ground speed on each stretch; inside it the drone climbs and lands vertically.`,
      ...(planner.step > step + 1e-9
        ? [
            `Heights searched every ${fixed(planner.step, 1)} m, not ${fixed(step, 1)} m: the corridor is too tall for a finer grid.`,
          ]
        : []),
      ...(MissionAltitude.field(m) === 'aglM'
        ? [
            'Heights above the ground follow the terrain between waypoints, so following a profile over rough ground takes more added waypoints than in a mission flown above launch.',
          ]
        : []),
    ],
    ...(perf.drone ? { drone: { id: perf.drone.id ?? '', version: perf.drone.version ?? 0 } } : {}),
  };
  return { data, reasons };
}
