import {
  AutoHeight,
  LinkKind,
  Mission,
  MissionAltitude,
  distanceM,
  interpolate,
  linksEvaluated,
} from '@rfmap/ui-kit';
import type {
  FpvAltitudeCorridor,
  FpvAltitudeData,
  FpvAltitudeOptions,
  FpvAltitudeWaypoint,
} from '../core/fpv-altitude';
import { elevation } from './mock-terrain';
import { LIBRARY } from './reference-case';

const C = 299792458;

interface Point {
  lat: number;
  lon: number;
  /** Height in the mission's reference */
  h: number;
  label?: string;
}

/** Distances along the route: every step and every point, like the module. */
function distances(cum: number[], stepM: number): number[] {
  const total = cum.at(-1) ?? 0;
  const at = [...cum];
  for (let s = stepM; s < total; s += stepM) at.push(s);
  return [...new Set(at.map((v) => Math.round(v * 1000) / 1000))].sort((a, b) => a - b);
}

/**
 * Lowest height above ground at the end of pilot → p where `fraction` of the first Fresnel zone
 * clears the mock terrain on a flat earth. Null when nothing up to `maxAgl` does.
 */
function linkFloorAgl(
  pilot: { lat: number; lon: number; antennaMsl: number },
  p: { lat: number; lon: number },
  lambda: number,
  fraction: number,
  maxAgl: number,
): number | null {
  const d = distanceM(pilot, p);
  if (d < 1) return null;
  const ground = elevation(p.lat, p.lon);
  const n = Math.max(8, Math.ceil(d / 60));
  const clear = (agl: number) => {
    for (let i = 1; i < n; i++) {
      const t = i / n;
      const q = interpolate(pilot, p, t);
      const ray = pilot.antennaMsl + (ground + agl - pilot.antennaMsl) * t;
      const r = Math.sqrt((lambda * d * t * d * (1 - t)) / d);
      if (ray - elevation(q.lat, q.lon) < fraction * r) return false;
    }
    return true;
  };
  if (!clear(maxAgl)) return null;
  let lo = 0;
  let hi = maxAgl;
  if (clear(0)) return 0;
  while (hi - lo > 0.5) {
    const mid = (lo + hi) / 2;
    if (clear(mid)) hi = mid;
    else lo = mid;
  }
  return hi;
}

function lowestFrequency(
  m: Mission,
): { link: LinkKind; radio: string; frequencyHz: number } | null {
  let best: { link: LinkKind; radio: string; frequencyHz: number } | null = null;
  for (const kind of linksEvaluated(m.evaluate)) {
    const setup = m.links?.[kind];
    const radio = LIBRARY.radios.find((r) => r.id === setup?.radio);
    if (radio?.frequencyHz && (!best || radio.frequencyHz < best.frequencyHz))
      best = { link: kind, radio: radio.name ?? radio.id ?? '', frequencyHz: radio.frequencyHz };
  }
  return best;
}

/** The options with their defaults; the step keeps the module's 10–2000 m bounds, so sampling ends. */
function optionsOf(m: Mission): Required<FpvAltitudeOptions> {
  const o: Required<FpvAltitudeOptions> = {
    stepM: 100,
    clearanceM: 30,
    takeoffM: 200,
    ceilingAglM: 120,
    linkFloor: true,
    fresnelFraction: 0.6,
    maxAglM: 500,
    autoHeight: true,
    ...(m.modules?.['fpv.altitude'] as FpvAltitudeOptions | undefined),
  };
  o.stepM = Number.isFinite(o.stepM) ? Math.min(2000, Math.max(10, o.stepM)) : 100;
  return o;
}

function emptyCorridor(o: Required<FpvAltitudeOptions>): FpvAltitudeCorridor {
  return {
    stepM: o.stepM,
    clearanceM: o.clearanceM,
    takeoffM: o.takeoffM,
    ceilingAglM: o.ceilingAglM,
    linkFloor: o.linkFloor,
    fresnelFraction: o.fresnelFraction,
    s: [],
    lat: [],
    lon: [],
    leg: [],
    groundMslM: [],
    plannedMslM: [],
    floorMslM: [],
    terrainFloorMslM: [],
    linkFloorMslM: [],
    ceilingMslM: [],
    fresnelRadiusM: [],
  };
}

/** The route as points from the pilot, with the distance along it to each. */
class Path {
  readonly cum = [0];

  constructor(readonly pts: Point[]) {
    for (let i = 1; i < pts.length; i++)
      this.cum.push(this.cum[i - 1] + distanceM(pts[i - 1], pts[i]));
  }

  legAt(s: number): number {
    let leg = 0;
    while (leg + 2 < this.pts.length && s > this.cum[leg + 1]) leg++;
    return leg;
  }

  /** Fraction of leg `leg` at distance s, clamped to the leg. */
  t(leg: number, s: number): number {
    const span = this.cum[leg + 1] - this.cum[leg];
    return span > 0 ? Math.min(1, Math.max(0, (s - this.cum[leg]) / span)) : 0;
  }

  at(leg: number, s: number): { lat: number; lon: number } {
    return interpolate(this.pts[leg], this.pts[leg + 1], this.t(leg, s));
  }
}

interface Link {
  pilot: { lat: number; lon: number; antennaMsl: number };
  lambda: number;
}

/** Appends the corridor sample at distance s along the path. */
function addSample(
  c: FpvAltitudeCorridor,
  path: Path,
  s: number,
  o: Required<FpvAltitudeOptions>,
  link: Link,
  msl: (h: number, ground: number) => number,
): void {
  const leg = path.legAt(s);
  const t = path.t(leg, s);
  const p = path.at(leg, s);
  const ground = elevation(p.lat, p.lon);
  const d = distanceM(link.pilot, p);
  const ramp = o.takeoffM > 0 ? Math.min(1, d / o.takeoffM) : 1;
  const near = [-o.stepM / 2, 0, o.stepM / 2].map((ds) => {
    const q = path.at(leg, s + ds);
    return elevation(q.lat, q.lon);
  });
  const terrainFloor = Math.max(...near) + o.clearanceM * ramp;
  const agl =
    o.linkFloor && link.lambda > 0
      ? linkFloorAgl(link.pilot, p, link.lambda, o.fresnelFraction, o.maxAglM)
      : null;
  const linkFloor = agl === null ? null : ground + agl;
  const a = path.pts[leg].h;
  const b = path.pts[leg + 1].h;
  c.s.push(s);
  c.lat.push(p.lat);
  c.lon.push(p.lon);
  c.leg.push(leg);
  c.groundMslM.push(ground);
  c.plannedMslM.push(msl(a + (b - a) * t, ground));
  c.terrainFloorMslM.push(terrainFloor);
  c.linkFloorMslM.push(linkFloor);
  c.floorMslM.push(Math.max(terrainFloor, linkFloor ?? -Infinity));
  c.ceilingMslM.push(ground + o.ceilingAglM);
  c.fresnelRadiusM.push(link.lambda > 0 && d >= 1 ? 0.5 * Math.sqrt(link.lambda * d) : 0);
}

function summaryOf(c: FpvAltitudeCorridor): FpvAltitudeData['summary'] {
  const margin = c.s.map((_, i) => c.plannedMslM[i] - c.floorMslM[i]);
  const iGround = c.groundMslM.indexOf(Math.max(...c.groundMslM));
  const iPlan = c.plannedMslM.indexOf(Math.max(...c.plannedMslM));
  const iMargin = margin.indexOf(Math.min(...margin));
  return {
    highestGroundMslM: c.groundMslM[iGround],
    highestGroundAtM: c.s[iGround],
    highestPlannedMslM: c.plannedMslM[iPlan],
    highestPlannedAtM: c.s[iPlan],
    lowestMarginM: margin[iMargin],
    lowestMarginAtM: c.s[iMargin],
  };
}

/** The corridor along the mission's route as planned, with what it was built from. */
function corridorOf(m: Mission) {
  const o = optionsOf(m);
  const ref = MissionAltitude.ref(m);
  const launch = elevation(m.pilot.lat, m.pilot.lon);
  const pts: Point[] = [
    { lat: m.pilot.lat, lon: m.pilot.lon, h: m.pilot.aglM },
    ...m.waypoints.map((w) => ({
      lat: w.lat,
      lon: w.lon,
      h: MissionAltitude.height(m, w),
      label: w.label,
    })),
  ];
  if (m.returnToPilot !== false) pts.push({ ...pts[0] });
  const path = new Path(pts);
  const fresnel = lowestFrequency(m);
  const link: Link = {
    pilot: { lat: m.pilot.lat, lon: m.pilot.lon, antennaMsl: launch + m.pilot.aglM },
    lambda: fresnel ? C / fresnel.frequencyHz : 0,
  };
  const msl = (h: number, ground: number) => (ref === 'launch' ? launch + h : ground + h);
  const c = emptyCorridor(o);
  for (const s of distances(path.cum, o.stepM)) addSample(c, path, s, o, link, msl);
  return { o, ref, launch, path, fresnel, link, msl, c };
}

/**
 * Mock of route.prepare in modules/fpv/altitude: every automatic waypoint gets the highest corridor
 * floor on its two legs (level legs between free or pilot ends, the line from a manual end
 * otherwise), capped by the ceiling at the waypoint.
 */
export function fpvAltitudePrepareMock(m: Mission): {
  mission: Mission;
  autoHeights: AutoHeight[];
} {
  const free = m.waypoints.map((w) => MissionAltitude.isAuto(m, w));
  if (optionsOf(m).autoHeight === false || !free.some(Boolean))
    return { mission: m, autoHeights: [] };
  const { o, ref, launch, path, fresnel, c } = corridorOf(m);
  const n = m.waypoints.length;
  const toRef = (msl: number, ground: number) => msl - (ref === 'launch' ? launch : ground);
  const isFree = (p: number) => p >= 1 && p <= n && free[p - 1];
  const fixedAt = (p: number) => (p === 0 || p === n + 1 ? null : path.pts[p].h);
  const need = new Array<number>(n).fill(-Infinity);
  const by = new Array<string>(n).fill('terrain');
  // Waypoints with a leg where no height up to maxAglM clears the link.
  const noLink = new Array<boolean>(n).fill(false);
  const raise = (p: number, other: number, toward: number, floor: number, limit: string) => {
    const h = fixedAt(other);
    let v = floor;
    // A manual end right by this sample cannot be lifted over the floor from here.
    if (!isFree(other) && h !== null) v = toward < 0.05 ? -Infinity : h + (floor - h) / toward;
    if (v > need[p - 1]) {
      need[p - 1] = v;
      by[p - 1] = limit;
    }
  };
  c.s.forEach((s, k) => {
    const leg = c.leg[k];
    const t = path.t(leg, s);
    const ground = c.groundMslM[k];
    const floor = toRef(c.floorMslM[k], ground);
    const limit =
      c.linkFloorMslM[k] !== null && c.linkFloorMslM[k]! > c.terrainFloorMslM[k]
        ? (fresnel?.link ?? 'control')
        : 'terrain';
    if (isFree(leg + 1)) raise(leg + 1, leg, t, floor, limit);
    if (isFree(leg)) raise(leg, leg + 1, 1 - t, floor, limit);
    if (o.linkFloor && c.fresnelRadiusM[k] > 0 && c.linkFloorMslM[k] === null) {
      if (leg < n) noLink[leg] = true;
      if (leg >= 1 && leg - 1 < n) noLink[leg - 1] = true;
    }
  });
  const field = MissionAltitude.field(m);
  const heights = m.waypoints.map((w, i) => {
    if (!free[i] || !Number.isFinite(need[i])) return null;
    const ground = elevation(w.lat, w.lon);
    const ceiling = toRef(ground + o.ceilingAglM, ground);
    // Whole metres: up for the floor, never over the ceiling; rounding down to it is the ceiling's too.
    return Math.min(Math.ceil(need[i]), Math.floor(ceiling));
  });
  // A leg whose line, with the heights set, goes over the ceiling blocks its free ends.
  const heightAt = (p: number) =>
    p >= 1 && p <= n && heights[p - 1] !== null ? heights[p - 1]! : path.pts[p].h;
  const overCeiling = new Array<boolean>(n).fill(false);
  c.s.forEach((s, k) => {
    const leg = c.leg[k];
    const t = path.t(leg, s);
    const line = heightAt(leg) + (heightAt(leg + 1) - heightAt(leg)) * t;
    if (line <= toRef(c.ceilingMslM[k], c.groundMslM[k]) + 0.05) return;
    if (leg >= 1 && leg <= n) overCeiling[leg - 1] = true;
    if (leg < n) overCeiling[leg] = true;
  });
  const autoHeights: AutoHeight[] = [];
  const waypoints = m.waypoints.map((w, i) => {
    const heightM = heights[i];
    if (heightM === null) return w;
    const capped = heightM < need[i];
    const ceiling = capped || (overCeiling[i] && !noLink[i]);
    autoHeights.push({
      index: i,
      lat: w.lat,
      lon: w.lon,
      field,
      heightM,
      module: 'fpv.altitude',
      limitedBy: ceiling ? 'ceiling' : by[i],
      blocked: capped || noLink[i] || overCeiling[i],
    });
    return { ...MissionAltitude.withHeight(m, w, heightM), heightSource: 'auto' as const };
  });
  return { mission: { ...m, waypoints }, autoHeights };
}

/** Mock of modules/fpv/altitude: the corridor along the route (heights interpolated in the mission's reference). */
export function fpvAltitudeMock(m: Mission): FpvAltitudeData {
  const { o, ref, launch, path, fresnel, link, msl, c } = corridorOf(m);
  const waypoints: FpvAltitudeWaypoint[] = m.waypoints.map((w, i) => {
    const ground = elevation(w.lat, w.lon);
    const mslM = msl(MissionAltitude.height(m, w), ground);
    return {
      index: i + 1,
      label: w.label,
      s: path.cum[i + 1],
      groundMslM: ground,
      mslM,
      aglM: mslM - ground,
      aboveLaunchM: mslM - launch,
    };
  });
  return {
    kFactor: 4 / 3,
    notes: [`Sampled every ${o.stepM} m along the route and at every waypoint (mock).`],
    reference: ref,
    pilot: {
      lat: m.pilot.lat,
      lon: m.pilot.lon,
      groundMslM: launch,
      antennaMslM: link.pilot.antennaMsl,
    },
    corridor: c,
    waypoints,
    summary: summaryOf(c),
    ...(fresnel ? { fresnel } : {}),
  };
}
