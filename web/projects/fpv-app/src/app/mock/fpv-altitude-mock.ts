import {
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

/** Mock of modules/fpv/altitude: the corridor along the route (heights interpolated in the mission's reference). */
export function fpvAltitudeMock(m: Mission): FpvAltitudeData {
  const o: Required<FpvAltitudeOptions> = {
    stepM: 100,
    clearanceM: 30,
    takeoffM: 200,
    ceilingAglM: 120,
    linkFloor: true,
    fresnelFraction: 0.6,
    maxAglM: 500,
    ...((m.modules?.['fpv.altitude'] as FpvAltitudeOptions | undefined) ?? {}),
  };
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
  const cum = [0];
  for (let i = 1; i < pts.length; i++) cum.push(cum[i - 1] + distanceM(pts[i - 1], pts[i]));
  const fresnel = lowestFrequency(m);
  const lambda = fresnel ? C / fresnel.frequencyHz : 0;
  const pilot = { lat: m.pilot.lat, lon: m.pilot.lon, antennaMsl: launch + m.pilot.aglM };
  const msl = (h: number, ground: number) => (ref === 'launch' ? launch + h : ground + h);

  const c: FpvAltitudeCorridor = {
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
  for (const s of distances(cum, o.stepM)) {
    let leg = 0;
    while (leg + 2 < pts.length && s > cum[leg + 1]) leg++;
    const span = cum[leg + 1] - cum[leg];
    const t = span > 0 ? Math.min(1, (s - cum[leg]) / span) : 0;
    const p = interpolate(pts[leg], pts[leg + 1], t);
    const ground = elevation(p.lat, p.lon);
    const d = distanceM(m.pilot, p);
    const ramp = o.takeoffM > 0 ? Math.min(1, d / o.takeoffM) : 1;
    const near = [-o.stepM / 2, 0, o.stepM / 2].map((ds) => {
      const tt = span > 0 ? Math.min(1, Math.max(0, (s + ds - cum[leg]) / span)) : 0;
      const q = interpolate(pts[leg], pts[leg + 1], tt);
      return elevation(q.lat, q.lon);
    });
    const terrainFloor = Math.max(...near) + o.clearanceM * ramp;
    const agl =
      o.linkFloor && fresnel ? linkFloorAgl(pilot, p, lambda, o.fresnelFraction, o.maxAglM) : null;
    const linkFloor = agl === null ? null : ground + agl;
    c.s.push(s);
    c.lat.push(p.lat);
    c.lon.push(p.lon);
    c.leg.push(leg);
    c.groundMslM.push(ground);
    c.plannedMslM.push(msl(pts[leg].h + (pts[leg + 1].h - pts[leg].h) * t, ground));
    c.terrainFloorMslM.push(terrainFloor);
    c.linkFloorMslM.push(linkFloor);
    c.floorMslM.push(Math.max(terrainFloor, linkFloor ?? -Infinity));
    c.ceilingMslM.push(ground + o.ceilingAglM);
    c.fresnelRadiusM.push(lambda > 0 && d >= 1 ? 0.5 * Math.sqrt(lambda * d) : 0);
  }
  const waypoints: FpvAltitudeWaypoint[] = m.waypoints.map((w, i) => {
    const ground = elevation(w.lat, w.lon);
    const mslM = msl(MissionAltitude.height(m, w), ground);
    return {
      index: i + 1,
      label: w.label,
      s: cum[i + 1],
      groundMslM: ground,
      mslM,
      aglM: mslM - ground,
      aboveLaunchM: mslM - launch,
    };
  });
  const margin = c.s.map((_, i) => c.plannedMslM[i] - c.floorMslM[i]);
  const iGround = c.groundMslM.indexOf(Math.max(...c.groundMslM));
  const iPlan = c.plannedMslM.indexOf(Math.max(...c.plannedMslM));
  const iMargin = margin.indexOf(Math.min(...margin));
  return {
    kFactor: 4 / 3,
    notes: [`Sampled every ${o.stepM} m along the route and at every waypoint (mock).`],
    reference: ref,
    pilot: {
      lat: m.pilot.lat,
      lon: m.pilot.lon,
      groundMslM: launch,
      antennaMslM: pilot.antennaMsl,
    },
    corridor: c,
    waypoints,
    summary: {
      highestGroundMslM: c.groundMslM[iGround],
      highestGroundAtM: c.s[iGround],
      highestPlannedMslM: c.plannedMslM[iPlan],
      highestPlannedAtM: c.s[iPlan],
      lowestMarginM: margin[iMargin],
      lowestMarginAtM: c.s[iMargin],
    },
    ...(fresnel ? { fresnel } : {}),
  };
}
