import { Diffraction, Profile } from '../models/api.models';
import { EARTH_RADIUS_M } from '../tools/geo';

export const SPEED_OF_LIGHT = 299_792_458;

export interface ProfileInput {
  profile: Profile;
  /** Height above ground of the transmitter end (pilot) and the receiver end (drone). */
  fromAglM: number;
  toAglM: number;
  frequencyHz: number;
  /** Effective Earth radius factor; terrain is drawn with the Earth bulge for this k. Default 4/3. */
  kFactor?: number;
  diffraction?: Diffraction;
}

export interface ProfileSample {
  d: number;
  ground: number;
  /** ground + obstacle + earth bulge, the surface the ray must clear */
  surface: number;
  ray: number;
  r1: number;
}

export interface Intrusion {
  /** polygon in data space (d, h) where the surface enters the first Fresnel zone */
  polygon: [number, number][];
  maxD: number;
  maxSurface: number;
  /** ray − surface at the worst point (negative: line of sight blocked) */
  clearance: number;
}

export interface ProfileGeometry {
  lengthM: number;
  samples: ProfileSample[];
  fromMsl: number;
  toMsl: number;
  lambda: number;
  intrusions: Intrusion[];
  /** The most obstructive sample: smallest (ray − surface)/r1 */
  worst: ProfileSample & { clearanceM: number; clearanceRatio: number };
  minH: number;
  maxH: number;
}

/** First Fresnel radius at d1/d2 for wavelength λ. */
export function fresnelRadius(lambda: number, d1: number, d2: number): number {
  const d = d1 + d2;
  if (d <= 0 || d1 <= 0 || d2 <= 0) return 0;
  return Math.sqrt((lambda * d1 * d2) / d);
}

/** Pure geometry for the profile chart: terrain, direct ray, first Fresnel zone and obstruction. */
export function computeProfileGeometry(input: ProfileInput): ProfileGeometry {
  const pts = [...input.profile.points].sort((a, b) => a.distanceM - b.distanceM);
  if (pts.length < 2) throw new Error('A profile needs at least two points');
  const L = input.profile.lengthM ?? pts[pts.length - 1].distanceM;
  const k = input.kFactor ?? 4 / 3;
  const lambda = SPEED_OF_LIGHT / input.frequencyHz;
  const fromMsl = pts[0].groundMslM + input.fromAglM;
  const toMsl = pts[pts.length - 1].groundMslM + input.toAglM;

  const samples: ProfileSample[] = pts.map((p) => {
    const d1 = p.distanceM;
    const d2 = L - d1;
    const bulge = (d1 * d2) / (2 * k * EARTH_RADIUS_M);
    const ray = fromMsl + ((toMsl - fromMsl) * d1) / L;
    return { d: d1, ground: p.groundMslM, surface: p.groundMslM + (p.obstacleM ?? 0) + bulge, ray, r1: fresnelRadius(lambda, d1, d2) };
  });

  let worst = { ...samples[0], clearanceM: Infinity, clearanceRatio: Infinity };
  for (const s of samples.slice(1, -1)) {
    const clearanceM = s.ray - s.surface;
    const ratio = s.r1 > 0 ? clearanceM / s.r1 : Infinity;
    if (ratio < worst.clearanceRatio) worst = { ...s, clearanceM, clearanceRatio: ratio };
  }

  const intrusions: Intrusion[] = [];
  let run: ProfileSample[] = [];
  const flush = () => {
    if (run.length === 0) return;
    const top = run.map((s) => [s.d, Math.min(s.surface, s.ray + s.r1)] as [number, number]);
    const bottom = [...run].reverse().map((s) => [s.d, s.ray - s.r1] as [number, number]);
    const peak = run.reduce((m, s) => (s.surface - s.ray > m.surface - m.ray ? s : m), run[0]);
    intrusions.push({ polygon: [...top, ...bottom], maxD: peak.d, maxSurface: peak.surface, clearance: peak.ray - peak.surface });
    run = [];
  };
  for (const s of samples) {
    if (s.r1 > 0 && s.surface > s.ray - s.r1) run.push(s);
    else flush();
  }
  flush();

  const heights = samples.flatMap((s) => [s.surface, s.ray + s.r1, s.ray - s.r1]).concat([fromMsl, toMsl]);
  return {
    lengthM: L,
    samples,
    fromMsl,
    toMsl,
    lambda,
    intrusions,
    worst,
    minH: Math.min(...samples.map((s) => s.ground), ...heights),
    maxH: Math.max(...heights),
  };
}

/** "Nice" tick step for a range, e.g. 100 m for a 300 m span. */
export function niceStep(span: number, targetTicks = 4): number {
  const raw = span / Math.max(1, targetTicks);
  const pow = 10 ** Math.floor(Math.log10(raw));
  const n = raw / pow;
  const step = n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10;
  return step * pow;
}
