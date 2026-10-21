/**
 * DEV MOCK ONLY. The reference case "North ridge recon": leg 3 WP2 → B, A → drone 5.280 km,
 * 915 MHz video blocked by the 622 m ridge (−3.74 dB), 433 MHz control +6.01 dB, battery short.
 * Every number here is the full-precision value the server would send; the UI rounds.
 */
import { contours } from 'd3-contour';
import type { MultiPolygon } from 'geojson';
import {
  Antenna,
  Battery,
  BudgetTerm,
  CoverageGrid,
  Drone,
  Fix,
  LegResult,
  LinkKind,
  LinkResult,
  Mission,
  MissionPoint,
  Profile,
  RadioProfile,
  RouteResult,
  WeatherPoint,
  ForecastHorizon,
  ForecastTier,
  LinkEnd,
  MissingSpan,
  Reason,
  ResultForecastTier,
  linksEvaluated,
} from '@rfmap/ui-kit';
import { LOCAL, RIDGE_FRACTION, elevation, elevationLocal, toLatLon, toLocal } from './mock-terrain';

const M = '−';
const pt = (p: { x: number; y: number }, aglM: number, label?: string): MissionPoint => ({ ...toLatLon(p.x, p.y), aglM, ...(label ? { label } : {}) });

export const PILOT = pt(LOCAL.A, 2, 'A');
export const WP1 = pt(LOCAL.WP1, 120);
export const WP2 = pt(LOCAL.WP2, 120);
export const TARGET = pt(LOCAL.B, 120, 'B');
const T_FRACTION = 2.32 / 3.295;
export const TURN_BACK = pt({ x: LOCAL.WP1.x + (LOCAL.WP2.x - LOCAL.WP1.x) * T_FRACTION, y: LOCAL.WP1.y + (LOCAL.WP2.y - LOCAL.WP1.y) * T_FRACTION }, 120, 'T');
export const RIDGE_POINT = toLatLon(LOCAL.B.x * RIDGE_FRACTION, LOCAL.B.y * RIDGE_FRACTION);

export const LEG_LENGTHS = [3683.11, 3295.02, 3866.47];
export const A_TO_B_M = 5280.37;
const ROUTE_M = LEG_LENGTHS.reduce((a, b) => a + b, 0); // 10 844.6
const NEEDED_M = ROUTE_M + A_TO_B_M; // 16 125.0
const RANGE_M = 10198.3;

// ---------------- library ----------------
const ds = (reference: string) => ({ kind: 'datasheet' as const, reference });

export const LIBRARY: { radios: RadioProfile[]; antennas: Antenna[]; batteries: Battery[]; drones: Drone[] } = {
  radios: [
    { id: 'elrs-433-50hz', name: 'ELRS 433 MHz', vendor: 'ExpressLRS', version: 2, verified: true, kind: 'control', frequencyHz: 433e6, txPowerDbm: [20, 27, 30], sensitivity: { method: 'datasheet', dBm: -105, source: ds('ExpressLRS 3.x, 50 Hz LoRa mode') }, latencyMs: { typical: 20, max: 40 } },
    { id: 'elrs-915-100hz', name: 'ELRS 915 MHz', vendor: 'ExpressLRS', version: 2, verified: true, kind: 'control', frequencyHz: 915e6, txPowerDbm: [20, 27, 30], sensitivity: { method: 'datasheet', dBm: -105, source: ds('ExpressLRS 3.x, 100 Hz LoRa mode') }, latencyMs: { typical: 10, max: 20 } },
    { id: 'digital-915-20mhz', name: 'digital 915 MHz', vendor: 'Generic', version: 2, verified: true, kind: 'video-digital', frequencyHz: 915e6, txPowerDbm: [27, 30], sensitivity: { method: 'datasheet', dBm: -105, source: ds('Vendor datasheet v1.2, p. 14') }, latencyMs: { typical: 40, max: 80 } },
    { id: 'analog-5800', name: 'Analog 5.8 · 25–1000 mW', vendor: 'Generic', version: 1, verified: true, kind: 'video-analog', frequencyHz: 5.8e9, txPowerDbm: [14, 23, 27, 30], sensitivity: { method: 'computed', bandwidthHz: 18e6, noiseFigureDb: 8, requiredSnrDb: 6, source: ds('Receiver datasheet') }, latencyMs: { typical: 1 }, analogGrades: [{ label: 'clear', requiredSnrDb: 18 }, { label: 'noisy', requiredSnrDb: 10 }, { label: 'lost', requiredSnrDb: 4 }], extraNoiseDb: 2 },
  ],
  antennas: [
    { id: 'ground-dipole-5dbi', name: 'Ground dipole · 5.00 dBi', version: 1, verified: true, type: 'dipole', gainDbi: 5, polarization: 'linear-vertical', bandHz: [400e6, 960e6], source: ds('Antenna datasheet') },
    { id: 'air-dipole-2dbi', name: 'Drone dipole · 2.00 dBi', version: 1, verified: true, type: 'dipole', gainDbi: 2, polarization: 'linear-vertical', bandHz: [400e6, 960e6], source: ds('Antenna datasheet') },
    { id: 'patch-8dbi-915', name: 'Patch 915 · 8.00 dBi', version: 1, verified: false, type: 'patch', gainDbi: 8, polarization: 'linear-vertical', bandHz: [902e6, 928e6], beamwidthDeg: { horizontal: 70, vertical: 65 }, source: ds('Antenna datasheet') },
  ],
  batteries: [
    { id: 'liion-6s-6000', name: '6S Li-ion 6000 mAh', version: 1, verified: true, chemistry: 'li-ion', cellsSeries: 6, capacityAh: 6, nominalCellVoltage: 3.6, usableFraction: 0.8, temperatureDerating: [{ celsius: -10, factor: 0.78 }, { celsius: 0, factor: 0.88 }, { celsius: 20, factor: 1 }], source: ds('Cell datasheet, 21700 6S2P') },
    { id: 'lipo-6s-3300', name: '6S LiPo 3300 mAh', version: 1, verified: false, chemistry: 'lipo', cellsSeries: 6, capacityAh: 3.3, nominalCellVoltage: 3.7, usableFraction: 0.8, notes: 'Capacity from the pack label; not yet checked on the bench.', source: { kind: 'measurement', reference: 'Pack label, field test by a user' } },
  ],
  drones: [
    { id: 'fpv-10in-recon', name: '10-inch recon quad', version: 1, verified: true, massKg: 1.9, cruiseAirspeedMps: 10, cruisePowerW: 366, powerDensityExponent: 0, climb: { method: 'potential-energy', efficiency: 0.6 }, defaultRadios: { control: 'elrs-433-50hz', video: 'digital-915-20mhz' }, source: { kind: 'measurement', reference: 'Bench and flight log 2026-09' } },
  ],
};

export function exampleMission(): Mission {
  return {
    schema: 'mission/v1',
    name: 'North ridge recon',
    time: { mode: 'now' },
    pilot: { ...PILOT },
    waypoints: [{ ...WP1 }, { ...WP2 }, { ...TARGET }],
    returnToPilot: true,
    links: {
      control: { radio: 'elrs-433-50hz', txPowerDbm: 30, groundAntenna: 'ground-dipole-5dbi', airAntenna: 'air-dipole-2dbi', requiredMarginDb: 10 },
      video: { radio: 'digital-915-20mhz', txPowerDbm: 30, groundAntenna: 'ground-dipole-5dbi', airAntenna: 'air-dipole-2dbi', requiredMarginDb: 10 },
    },
    evaluate: 'both',
    drone: 'fpv-10in-recon',
    battery: 'liion-6s-6000',
    modules: { 'fpv.battery': {}, 'rf.weather': {}, 'fpv.example': {} },
    coverage: { radiusM: 8000, cellM: 100, droneAglM: 120 },
  };
}

// ---------------- weather ----------------
export function horizon(now = new Date()): ForecastHorizon {
  const run = new Date(now);
  run.setUTCMinutes(0, 0, 0);
  run.setUTCHours(Math.floor(run.getUTCHours() / 6) * 6);
  return {
    runUtc: run.toISOString(),
    hourlyUntilUtc: new Date(run.getTime() + 7 * 86400e3).toISOString(),
    longRangeUntilUtc: new Date(run.getTime() + 16 * 86400e3).toISOString(),
  };
}

export function tierFor(timeUtc: string | undefined, h = horizon()): ForecastTier {
  const t = timeUtc ? new Date(timeUtc).getTime() : Date.now();
  if (t <= new Date(h.hourlyUntilUtc!).getTime()) return 'hourly';
  if (t <= new Date(h.longRangeUntilUtc!).getTime()) return 'long-range';
  return 'none';
}

/**
 * Forecast for a time, and for a place when `at` is given. Without a place it is the reference
 * value at the pilot (what route results carry). With one, the wind freshens and veers towards
 * the north-east, it cools with height and rain sets in east of the ridge, so the weather along a
 * route changes the way a real forecast does.
 */
export function weatherAt(timeUtc: string | undefined, stale: boolean, at?: { lat: number; lon: number }): WeatherPoint {
  const h = horizon();
  const tier = tierFor(timeUtc, h);
  const valid = timeUtc ?? new Date().toISOString();
  if (tier === 'none') {
    return { model: 'ISA standard atmosphere', tier, validUtc: valid, temperatureC: 15, pressureHpa: 1013.25, effectiveEarthFactor: 4 / 3, airDensityKgM3: 1.225, refractivityGradientPerKm: -39.2 };
  }
  const runUtc = stale ? new Date(Date.now() - 6.2 * 3600e3).toISOString() : h.runUtc;
  const base: WeatherPoint = {
    model: tier === 'hourly' ? 'ICON-D2 + GFS (Open-Meteo, self-hosted)' : 'ECMWF IFS open data (long-range)',
    runUtc,
    validUtc: valid,
    tier,
    temperatureC: 4.2,
    pressureHpa: 1009.6,
    relativeHumidity: 81,
    windSpeedMps: 7.4,
    windFromDeg: 292,
    refractivityGradientPerKm: -41.7,
    effectiveEarthFactor: 1.3617,
    airDensityKgM3: 1.18312,
    stale,
  };
  return at ? { ...base, ...localWeather(at, valid) } : base;
}

/** How the mock weather differs from the pilot's at another place and hour. */
function localWeather(at: { lat: number; lon: number }, validUtc: string): Partial<WeatherPoint> {
  const { x, y } = toLocal(at.lat, at.lon);
  const hours = (new Date(validUtc).getTime() - Date.now()) / 3600e3;
  const swing = Math.sin(hours / 3);
  const round = (v: number, d: number) => Number(v.toFixed(d));
  const ground = elevation(at.lat, at.lon);
  return {
    windSpeedMps: round(Math.max(0.5, 7.4 + 0.35 * x + 0.25 * y + 1.2 * swing), 1),
    windFromDeg: round((((292 + 3 * y - 2 * x) % 360) + 360) % 360, 0),
    temperatureC: round(4.2 - 0.0065 * (ground - elevation(PILOT.lat, PILOT.lon)) - 0.8 * swing, 1),
    precipitationMmPerHour: round(Math.max(0, 0.25 * (x - 1.5) + 0.3 * swing), 1),
    relativeHumidity: round(Math.min(100, 81 + 2 * y), 0),
    surfaceElevationM: round(ground, 0),
  };
}

// ---------------- link results ----------------
/** Ground profile pilot → `to` (always ground first; `link()` reorders from the transmitter). */
function profileBetween(a: MissionPoint, b: MissionPoint, n = 160): Profile {
  const la = toLocal(a.lat, a.lon);
  const lb = toLocal(b.lat, b.lon);
  const L = Math.hypot(lb.x - la.x, lb.y - la.y) * 1000;
  const points = [];
  for (let i = 0; i <= n; i++) {
    const t = i / n;
    const x = la.x + (lb.x - la.x) * t;
    const y = la.y + (lb.y - la.y) * t;
    const ll = toLatLon(x, y);
    points.push({ distanceM: L * t, groundMslM: elevationLocal(x, y), lat: ll.lat, lon: ll.lon });
  }
  return { source: 'Copernicus DEM GLO-30 (mock)', lengthM: L, stepM: L / n, points };
}

function fspl(distanceM: number, f: number): number {
  return 20 * Math.log10(distanceM / 1000) + 20 * Math.log10(f / 1e6) + 32.44778;
}

const TERMS_FIXED = (radio: string): BudgetTerm[] => [
  { id: 'tx-power', label: 'TX power', valueDb: -30, source: `radio profile ${radio} rev 2` },
  { id: 'ground-antenna', label: 'Ground antenna', valueDb: -5, source: 'antenna ground-dipole-5dbi rev 1' },
  { id: 'air-antenna', label: 'Drone antenna', valueDb: -2, source: 'antenna air-dipole-2dbi rev 1' },
];

/** Where along the leg the worst sample sits, and the two ends of the link there. */
interface LinkGeometry {
  pilot: MissionPoint;
  drone: MissionPoint;
  alongLegM: number;
}

interface LinkSpec {
  kind: LinkKind;
  f: number;
  rayM: number;
  fsplDb: number;
  diffDb: number;
  nu: number;
  r1: number;
  los: boolean;
  clearanceM: number;
  /** Ground first (pilot → drone) */
  profile: Profile;
  geo: LinkGeometry;
  /** Obstruction distance from the pilot, m */
  edgeFromPilotM?: number;
  extraNotes?: { what: string; detail: string }[];
}

const end = (p: MissionPoint): LinkEnd => {
  const groundMslM = elevation(p.lat, p.lon);
  return { lat: p.lat, lon: p.lon, aglM: p.aglM, groundMslM, antennaMslM: groundMslM + p.aglM };
};

/** Contract order: profile and diffraction distances run from the transmitter (video: drone → pilot). */
function fromTransmitter(profile: Profile, airTx: boolean): Profile {
  if (!airTx) return profile;
  const L = profile.lengthM ?? profile.points.at(-1)!.distanceM;
  return { ...profile, points: [...profile.points].reverse().map((p) => ({ ...p, distanceM: L - p.distanceM })) };
}

function link(spec: LinkSpec, tier: ResultForecastTier): LinkResult {
  const radio = spec.kind === 'video' ? 'digital-915-20mhz' : 'elrs-433-50hz';
  const airTx = spec.kind === 'video';
  const terms: BudgetTerm[] = [
    ...TERMS_FIXED(radio),
    { id: 'fspl', label: 'Free space', valueDb: spec.fsplDb, source: 'ITU-R P.525 · geodesic distance on WGS84' },
  ];
  if (spec.diffDb > 0) terms.push({ id: 'diffraction', label: 'Diffraction', valueDb: spec.diffDb, source: 'ITU-R P.526 · Bullington, exact Fresnel integrals' });
  const received = -terms.reduce((s, t) => s + t.valueDb, 0);
  const required = -95;
  const margin = received - required;
  const edgeFromPilot = spec.edgeFromPilotM ?? spec.rayM * RIDGE_FRACTION;
  const edgeFromTx = airTx ? spec.rayM - edgeFromPilot : edgeFromPilot;
  const notes = [
    { what: 'Input data error (not ours)', detail: `Elevation ±4 m (Copernicus GLO-30, LE90). Worst case ridge 4 m lower: ${margin < 0 ? 'still blocked' : 'still clear'}. Verdict: certain.` },
    ...(tier === 'none' ? [{ what: 'Beyond the forecast horizon', detail: 'No forecast exists for this time. Standard atmosphere (ISA, k = 4/3) is used; we do not invent weather.' }] : []),
    ...(tier === 'long-range' ? [{ what: 'Long-range forecast', detail: 'Refraction and air density come from coarser long-range models.' }] : []),
    ...(tier === 'off' ? [{ what: 'Weather off', detail: 'rf.weather is not enabled for this mission: standard atmosphere, k = 4/3.' }] : []),
    ...(spec.extraNotes ?? []),
  ];
  const { pilot, drone, alongLegM } = spec.geo;
  return {
    kind: spec.kind,
    frequencyHz: spec.f,
    txPowerDbm: 30,
    distanceM: spec.rayM,
    rayLengthM: spec.rayM,
    terms,
    receivedDbm: received,
    sensitivityDbm: -105,
    requiredMarginDb: 10,
    requiredDbm: required,
    marginDb: margin,
    reachable: margin >= 0,
    latencyMs: spec.kind === 'video' ? 40 : 20,
    ends: { ground: end(pilot), air: end(drone), transmitter: airTx ? 'air' : 'ground' },
    earth: { effectiveEarthFactor: 4 / 3, source: tier === 'off' ? 'standard atmosphere' : 'rf.weather' },
    forecastTier: tier,
    sample: { lat: drone.lat, lon: drone.lon, aglM: drone.aglM, alongLegM },
    diffraction: {
      lineOfSight: spec.los,
      nu: spec.nu,
      edgeHeightM: 622,
      d1M: edgeFromTx,
      d2M: spec.rayM - edgeFromTx,
      lossDb: spec.diffDb,
      minFresnelClearance: spec.clearanceM / spec.r1,
      firstFresnelRadiusM: spec.r1,
      dominant: { distanceM: edgeFromTx, groundMslM: 622, lat: RIDGE_POINT.lat, lon: RIDGE_POINT.lon },
    },
    uncertainty: notes,
    profile: fromTransmitter(spec.profile, airTx),
    model: 'rf-core 1.3.0 · ITU-R P.526 single edge (Bullington) · k = 4/3 · forest off',
  };
}

function clearLink(kind: LinkKind, pilot: MissionPoint, drone: MissionPoint, alongLegM: number, tier: ResultForecastTier): LinkResult {
  const f = kind === 'video' ? 915e6 : 433e6;
  const profile = profileBetween(pilot, drone);
  const d = profile.lengthM ?? 1;
  const r = link({ kind, f, rayM: d, fsplDb: fspl(d, f), diffDb: 0, nu: -2.1, r1: Math.sqrt(((299792458 / f) * d) / 4), los: true, clearanceM: 60, profile, geo: { pilot, drone, alongLegM } }, tier);
  if (r.diffraction) r.diffraction = { lineOfSight: true, lossDb: 0, minFresnelClearance: 1.4, nu: -2.1 };
  return r;
}

/** The ridge link A → drone at B (blocked, or cleared at 345 m). */
function ridgeLinks(mission: Mission, drone: MissionPoint, fixed: boolean, alongLegM: number, tier: ResultForecastTier) {
  const profile = profileBetween(mission.pilot, drone);
  profile.lengthM = A_TO_B_M;
  const geo = { pilot: mission.pilot, drone, alongLegM };
  const video = fixed
    ? link({ kind: 'video', f: 915e6, rayM: A_TO_B_M, fsplDb: 106.12874, diffDb: 0.54891, nu: -0.6814, r1: 20.7934, los: true, clearanceM: 10.21, profile, geo }, tier)
    : link({ kind: 'video', f: 915e6, rayM: A_TO_B_M, fsplDb: 106.12874, diffDb: 29.61391, nu: 6.8013, r1: 20.7934, los: false, clearanceM: -99.86, profile, geo }, tier);
  const control = fixed
    ? link({ kind: 'control', f: 433e6, rayM: A_TO_B_M, fsplDb: 99.63112, diffDb: 0.31107, nu: -0.4689, r1: 30.2214, los: true, clearanceM: 10.21, profile, geo }, tier)
    : link({ kind: 'control', f: 433e6, rayM: A_TO_B_M, fsplDb: 99.63112, diffDb: 26.35847, nu: 4.6795, r1: 30.2214, los: false, clearanceM: -99.86, profile, geo }, tier);
  return { control, video };
}

/** The part of the DEM that is "not loaded" in the missing-elevation scenario: around the end of leg 3. */
function missingArea(): [number, number, number, number] {
  const f0 = (LEG_LENGTHS[2] - 2400) / LEG_LENGTHS[2];
  const s = toLocal(WP2.lat, WP2.lon);
  const e = toLocal(TARGET.lat, TARGET.lon);
  const start = { x: s.x + (e.x - s.x) * f0, y: s.y + (e.y - s.y) * f0 };
  const xs = [start.x, e.x];
  const ys = [start.y, e.y];
  const sw = toLatLon(Math.min(...xs) - 0.3, Math.min(...ys) - 0.3);
  const ne = toLatLon(Math.max(...xs) + 0.6, Math.max(...ys) + 0.6);
  return [sw.lon, sw.lat, ne.lon, ne.lat];
}

const inBbox = (b: [number, number, number, number], p: { lat: number; lon: number }) => p.lon >= b[0] && p.lon <= b[2] && p.lat >= b[1] && p.lat <= b[3];

/** Spans (m from the pilot) of the pilot → drone path that fall inside the missing area. */
function missingSpans(pilot: MissionPoint, drone: MissionPoint, bbox: [number, number, number, number]): MissingSpan[] {
  const a = toLocal(pilot.lat, pilot.lon);
  const b = toLocal(drone.lat, drone.lon);
  const L = Math.hypot(b.x - a.x, b.y - a.y) * 1000;
  const n = 200;
  const spans: MissingSpan[] = [];
  let open: number | null = null;
  for (let i = 0; i <= n; i++) {
    const t = i / n;
    const inside = inBbox(bbox, toLatLon(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
    if (inside && open === null) open = L * t;
    if (!inside && open !== null) {
      spans.push({ fromM: open, toM: L * t });
      open = null;
    }
  }
  if (open !== null) spans.push({ fromM: open, toM: L });
  return spans;
}

function missingLegData(mission: Mission, from: MissionPoint, to: MissionPoint, lengthM: number, stepM = 100): Pick<LegResult, 'missing' | 'missingData' | 'samples'> {
  const bbox = missingArea();
  const total = Math.ceil(lengthM / stepM) + 1;
  const a = toLocal(from.lat, from.lon);
  const b = toLocal(to.lat, to.lon);
  const missing: NonNullable<LegResult['missing']> = [];
  let missingCount = 0;
  for (let i = 0; i < total; i++) {
    const t = Math.min(1, (i * stepM) / lengthM);
    const ll = toLatLon(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    const drone = { ...ll, aglM: from.aglM + (to.aglM - from.aglM) * t };
    const spans = missingSpans(mission.pilot, drone, bbox);
    if (!spans.length) continue;
    missingCount++;
    if (missing.length < 20 && (missingCount - 1) % 4 === 0) missing.push({ alongLegM: lengthM * t, lat: ll.lat, lon: ll.lon, missing: spans });
  }
  return {
    missing,
    missingData: { bbox, source: 'Copernicus DEM GLO-30 (mock)' },
    samples: { stepM, total, evaluated: total - missingCount, missingData: missingCount, overPilot: 0 },
  };
}

// ---------------- fixes ----------------
export function fixAltitude(mission: Mission): Fix & { note: string } {
  const wps = mission.waypoints.map((w, i, arr) => (i === arr.length - 1 ? { ...w, aglM: 345 } : w));
  return {
    id: 'leg3-345m-agl',
    title: 'Fly leg 3 at 345 m AGL',
    patch: { waypoints: wps },
    preview: [
      { label: 'Leg 3 height', now: '120 m', after: '345 m', trend: 'neutral' },
      { label: 'Line of sight at ridge', now: `${M}100 m`, after: '+10 m', trend: 'better' },
      { label: 'Ridge diffraction', now: '29.61 dB', after: '0.55 dB', trend: 'better' },
      { label: 'Video margin', now: `${M}3.74 dB`, after: '+25.32 dB', trend: 'better' },
      { label: 'Battery need', now: '16.1 km', after: 'with climb, after Apply', trend: 'caution' },
    ],
    note: 'The ridge is cleared by 10 m; part of the Fresnel zone still touches it, so a small diffraction loss stays. The battery estimate for the climb comes from the battery module after Apply.',
  };
}

export function fixTurnBack(mission: Mission): Fix & { note: string } {
  return {
    id: 'turn-back-t',
    title: 'Turn back at T',
    patch: { waypoints: [mission.waypoints[0], { ...TURN_BACK }] },
    preview: [
      { label: 'Out along route', now: '10.8 km', after: '6.0 km', trend: 'neutral' },
      { label: 'Back to pilot', now: '5.3 km', after: '4.2 km', trend: 'neutral' },
      { label: 'Total', now: '16.1 km', after: '10.2 km', trend: 'better' },
      { label: 'Reaches B', now: 'yes', after: 'no', trend: 'worse' },
    ],
    note: 'T is 2.3 km into leg 2. The return fits the 10.2 km range exactly; leg 3 and target B are dropped from this flight.',
  };
}

export type MockScenario = 'normal' | 'missing-elevation' | 'module-failed' | 'stale-forecast';

/**
 * The /calc/route answer for the reference mission and its fixed / turn-back variants, in the
 * server's shapes: legs[] 0-based with the return leg last, rf-core reasons per leg with `link`,
 * module reasons route-wide (or on a leg with `leg`), modules[id] = the module's data.
 */
export function routeResult(mission: Mission, scenario: MockScenario): RouteResult {
  const evaluated = linksEvaluated(mission.evaluate);
  const last = mission.waypoints.at(-1);
  const turnback = last?.label === 'T';
  const fixed = !turnback && (last?.aglM ?? 0) >= 345;
  const weatherOn = !!mission.modules?.['rf.weather'];
  const timeUtc = plannedUtc(mission);
  const tier: ResultForecastTier = weatherOn ? tierFor(timeUtc) : 'off';
  const weather = weatherOn ? weatherAt(timeUtc, scenario === 'stale-forecast') : undefined;
  const returning = mission.returnToPilot !== false;
  const legEnds = legEndsOf(mission, returning);
  const facts: RouteFacts = { mission, scenario, tier, evaluated, turnback, fixed, returning, legCount: legEnds.length };
  const legs: LegResult[] = legEnds.map((e, i) => legResult(facts, e, i));

  if (turnback && legs.length) {
    // a module reason with `leg` lands in that leg's reasons
    const lastOut = returning ? legs.length - 2 : legs.length - 1;
    legs[lastOut].reasons = [...(legs[lastOut].reasons ?? []), { module: 'fpv.core', severity: 'info', leg: lastOut, text: 'The flight ends at T and returns; leg 3 is not flown, so the ridge no longer matters.' }];
  }

  const result: RouteResult = {
    missionRevision: 1,
    inputHash: `mock-${routeVariant(turnback, fixed)}-${mission.evaluate}`,
    timeUtc: timeUtc ?? new Date().toISOString(),
    legs,
    reasons: [],
    weather,
    forecastTier: tier,
    library: [
      { kind: 'radio-profiles', id: 'elrs-433-50hz', version: 2, verified: true },
      { kind: 'radio-profiles', id: 'digital-915-20mhz', version: 2, verified: true },
      { kind: 'antennas', id: 'ground-dipole-5dbi', version: 1, verified: true },
      { kind: 'antennas', id: 'air-dipole-2dbi', version: 1, verified: true },
      { kind: 'drones', id: 'fpv-10in-recon', version: 1, verified: true },
      { kind: 'batteries', id: 'liion-6s-6000', version: 1, verified: true },
    ],
    modules: {},
    moduleFailures: [],
  };

  if (scenario === 'module-failed') {
    result.moduleFailures = [{ module: 'fpv.battery 1.0.4', error: 'hit its memory limit (1 GB)' }];
  } else {
    applyBattery(result, mission, turnback, fixed);
  }
  return result;
}

/** What every leg of one /calc/route answer depends on. */
interface RouteFacts {
  mission: Mission;
  scenario: MockScenario;
  tier: ResultForecastTier;
  evaluated: LinkKind[];
  turnback: boolean;
  fixed: boolean;
  returning: boolean;
  legCount: number;
}

interface LegEnds {
  from: MissionPoint;
  to: MissionPoint;
  fromName: string;
  toName: string;
}

/** The mission time when planned; undefined for "now". */
function plannedUtc(mission: Mission): string | undefined {
  return mission.time.mode === 'planned' ? mission.time.utc : undefined;
}

/** Legs pilot → waypoints, plus the return leg to the pilot when the mission returns. */
function legEndsOf(mission: Mission, returning: boolean): LegEnds[] {
  const pts = [mission.pilot, ...mission.waypoints];
  const last = mission.waypoints.at(-1);
  const legEnds: LegEnds[] = [];
  const nameOf = (p: MissionPoint, i: number) => p.label ?? (i === 0 ? 'Pilot' : `WP${i}`);
  for (let i = 0; i + 1 < pts.length; i++) legEnds.push({ from: pts[i], to: pts[i + 1], fromName: nameOf(pts[i], i), toName: nameOf(pts[i + 1], i + 1) });
  if (returning && last) legEnds.push({ from: last, to: mission.pilot, fromName: nameOf(last, pts.length - 1), toName: nameOf(mission.pilot, 0) });
  return legEnds;
}

/** Only the evaluated links, without the ones that have no result. */
function pickEvaluated(evaluated: LinkKind[], kinds: Partial<Record<LinkKind, LinkResult>>) {
  return Object.fromEntries(evaluated.map((k) => [k, kinds[k]]).filter(([, v]) => v));
}

function legLengthM(i: number, isReturn: boolean, turnback: boolean): number {
  if (isReturn) return turnback ? 4195.17 : A_TO_B_M;
  if (turnback && i === 1) return 2320.04;
  return LEG_LENGTHS[i] ?? 1;
}

/** Input hash tag of the mission variant: t = turn back at T, f = fixed altitude, b = base case. */
function routeVariant(turnback: boolean, fixed: boolean): 't' | 'f' | 'b' {
  if (turnback) return 't';
  return fixed ? 'f' : 'b';
}

function legResult(f: RouteFacts, e: LegEnds, i: number): LegResult {
  const isReturn = f.returning && i === f.legCount - 1;
  const lengthM = legLengthM(i, isReturn, f.turnback);
  const base = { index: i, from: e.fromName, to: e.toName, lengthM };
  // the worst point of a leg away from the pilot is its far end; of the return leg its start
  const drone = isReturn ? e.from : e.to;
  const alongLegM = isReturn ? 0 : lengthM;
  const ridgeLeg = f.turnback ? -1 : 2;
  const touchesB = !f.turnback && (i === ridgeLeg || isReturn);
  if (touchesB && f.scenario === 'missing-elevation') {
    return {
      ...base,
      status: 'missing-data',
      reasons: [{ module: 'rf-core', severity: 'blocked', text: `No elevation data ${isReturn ? 'around B' : 'past WP2'}: part of the paths to the drone are outside the loaded region. Those samples are not calculated: we do not guess the terrain.` }],
      ...missingLegData(f.mission, e.from, e.to, lengthM),
    };
  }
  if (!touchesB) {
    const worst = pickEvaluated(f.evaluated, { control: clearLink('control', f.mission.pilot, drone, alongLegM, f.tier), video: clearLink('video', f.mission.pilot, drone, alongLegM, f.tier) });
    return { ...base, worst, status: 'ok', reasons: [] };
  }
  const worst = pickEvaluated(f.evaluated, ridgeLinks(f.mission, drone, f.fixed, alongLegM, f.tier));
  const videoBlocked = !!worst['video'] && !worst['video'].reachable;
  return { ...base, worst, status: videoBlocked ? 'blocked' : 'ok', reasons: ridgeReasons(f.mission, videoBlocked, isReturn, f.fixed) };
}

/** rf-core reasons of a leg that ends (or, for the return, starts) at B behind the ridge. */
function ridgeReasons(mission: Mission, videoBlocked: boolean, isReturn: boolean, fixed: boolean): Reason[] {
  if (videoBlocked) {
    const where = isReturn ? 'at B, the start of the return' : 'at B';
    return [{ module: 'rf-core', severity: 'blocked', link: 'video', text: `Ridge 622 m blocks the video link by 100 m ${where}.`, ...(isReturn ? {} : { fix: fixAltitude(mission) }) }];
  }
  if (fixed) return [{ module: 'rf-core', severity: 'info', link: 'video', text: 'At 345 m AGL the line of sight passes 10 m over the ridge: diffraction 0.55 dB, video margin +25.32 dB.' }];
  return [];
}

/** fpv.battery data and reason: the route is too long for the pack; turning back at T fits. */
function applyBattery(result: RouteResult, mission: Mission, turnback: boolean, fixed: boolean): void {
  const usableWh = 103.68;
  const whPerM = usableWh / RANGE_M;
  result.modules = { 'fpv.battery': { usableWh, rangeM: RANGE_M } };
  if (turnback) {
    const out = 6003.13;
    const back = 4195.17;
    result.battery = { energyWh: 129.6, usableWh, neededWh: (out + back) * whPerM, rangeM: RANGE_M, neededM: out + back, shortfallM: 0, turnBack: { distanceAlongRouteM: out, returnM: back, lat: TURN_BACK.lat, lon: TURN_BACK.lon } };
    result.reasons = [{ module: 'fpv.battery', severity: 'info', text: 'Turn back at T: 6.0 km out and 4.2 km back fit the 10.2 km range.' }];
    return;
  }
  const climbM = fixed ? 196.4 : 0;
  const needed = NEEDED_M + climbM;
  const out = 6003.13;
  result.battery = {
    energyWh: 129.6,
    usableWh,
    neededWh: needed * whPerM,
    rangeM: RANGE_M,
    neededM: needed,
    shortfallM: needed - RANGE_M,
    turnBack: { distanceAlongRouteM: out, returnM: 4195.17, lat: TURN_BACK.lat, lon: TURN_BACK.lon },
  };
  result.reasons = [
    {
      module: 'fpv.battery',
      severity: 'warn',
      text: fixed
        ? `Route plus return with the climb to 345 m needs ${(needed / 1000).toFixed(1)} km; the pack gives 10.2 km at 36 km/h.`
        : 'Route 10.8 km plus 5.3 km back to the pilot needs 16.1 km; the pack gives 10.2 km at 36 km/h.',
      fix: fixTurnBack(mission),
    },
  ];
}

// ---------------- coverage ----------------
function knifeEdgeLossDb(nu: number): number {
  if (nu <= -0.78) return 0;
  return 6.9 + 20 * Math.log10(Math.sqrt((nu - 0.1) ** 2 + 1) + nu - 0.1);
}

const gridCache = new Map<string, CoverageGrid>();

function sensitivityDbm(r: RadioProfile | undefined): number {
  if (!r) return -105;
  const s = r.sensitivity;
  return s.method === 'datasheet' ? s.dBm : -174 + 10 * Math.log10(s.bandwidthHz) + s.noiseFigureDb + s.requiredSnrDb;
}

/**
 * How much better (+) or worse (−) a link setup is than the reference setup, in dB, and its frequency.
 * Lets the mock coverage react to scenario B (other radio, power or antenna) without a real model.
 */
export function linkBudgetDelta(mission: Mission, kind: LinkKind): { frequencyHz: number; biasDb: number } {
  const ref = exampleMission().links[kind]!;
  const cur = mission.links[kind] ?? ref;
  const radio = (id: string) => LIBRARY.radios.find((r) => r.id === id);
  const gain = (id: string) => LIBRARY.antennas.find((a) => a.id === id)?.gainDbi ?? 0;
  const refRadio = radio(ref.radio);
  const curRadio = radio(cur.radio) ?? refRadio;
  const frequencyHz = curRadio?.frequencyHz ?? (kind === 'video' ? 915e6 : 433e6);
  const biasDb =
    cur.txPowerDbm - ref.txPowerDbm +
    gain(cur.groundAntenna) - gain(ref.groundAntenna) +
    gain(cur.airAntenna) - gain(ref.airAntenna) +
    sensitivityDbm(refRadio) - sensitivityDbm(curRadio);
  return { frequencyHz, biasDb: Math.round(biasDb * 1000) / 1000 };
}

/** Margin grid around the pilot for one link, row-major north to south, null = below threshold handled by the client. */
export function coverageGrid(mission: Mission, kind: LinkKind): CoverageGrid {
  const link = linkBudgetDelta(mission, kind);
  const key = `${kind}:${mission.pilot.lat.toFixed(5)},${mission.pilot.lon.toFixed(5)}:${mission.coverage?.droneAglM ?? 120}:${link.frequencyHz}:${link.biasDb}`;
  const hit = gridCache.get(key);
  if (hit) return hit;
  const f = link.frequencyHz;
  const lambda = 299792458 / f;
  const radiusKm = (mission.coverage?.radiusM ?? 8000) / 1000;
  const cellKm = Math.max(0.05, (mission.coverage?.cellM ?? 100) / 1000);
  const n = Math.round((2 * radiusKm) / cellKm);
  const p0 = toLocal(mission.pilot.lat, mission.pilot.lon);
  const hA = elevationLocal(p0.x, p0.y) + mission.pilot.aglM;
  const droneAgl = mission.coverage?.droneAglM ?? 120;
  const margins: (number | null)[] = new Array(n * n);
  const samples = 48;
  for (let r = 0; r < n; r++) {
    const y = p0.y + radiusKm - (r + 0.5) * cellKm;
    for (let c = 0; c < n; c++) {
      const x = p0.x - radiusKm + (c + 0.5) * cellKm;
      const dKm = Math.hypot(x - p0.x, y - p0.y);
      if (dKm > radiusKm) {
        margins[r * n + c] = null;
        continue;
      }
      const dM = Math.max(30, dKm * 1000);
      const hB = elevationLocal(x, y) + droneAgl;
      const worstNu = worstNuOnPath({ x: p0.x, y: p0.y, h: hA }, { x, y, h: hB }, dM, lambda, samples);
      const extra = 30.7 * (dKm / 6.4) ** 2; // stands in for clutter / multipath in the mock
      const margin = 37 - fspl(dM, f) - knifeEdgeLossDb(worstNu) - extra + 95 + link.biasDb;
      margins[r * n + c] = margin;
    }
  }
  const nw = toLatLon(p0.x - radiusKm, p0.y + radiusKm);
  const se = toLatLon(p0.x + radiusKm, p0.y - radiusKm);
  const values = margins.map((m) => m ?? -999);
  const [zero] = contours().size([n, n]).thresholds([0])(values);
  const geo = zero as unknown as MultiPolygon;
  const limit: MultiPolygon = {
    type: 'MultiPolygon',
    coordinates: geo.coordinates.map((poly) =>
      poly.map((ring) =>
        ring.map(([cx, cy]) => {
          const ll = toLatLon(p0.x - radiusKm + cx * cellKm, p0.y + radiusKm - cy * cellKm);
          return [ll.lon, ll.lat];
        }),
      ),
    ),
  };
  const grid: CoverageGrid = {
    link: kind,
    pilot: { lat: mission.pilot.lat, lon: mission.pilot.lon },
    forecastTier: mission.modules?.['rf.weather'] ? tierFor(plannedUtc(mission)) : 'off',
    radiusM: radiusKm * 1000,
    droneAglM: droneAgl,
    origin: { lat: nw.lat, lon: nw.lon },
    cellM: cellKm * 1000,
    // the mock grid is regular in its local km plane, which maps linearly to degrees here
    cellLatDeg: (nw.lat - se.lat) / n,
    cellLonDeg: (se.lon - nw.lon) / n,
    cols: n,
    rows: n,
    bbox: [nw.lon, se.lat, se.lon, nw.lat],
    marginDb: margins,
    missingDataCells: 0,
    limitPolygon: limit,
  };
  gridCache.set(key, grid);
  return grid;
}

/** Worst knife-edge parameter ν over `samples` points of the straight path between two antennas (local km, MSL m). */
function worstNuOnPath(a: { x: number; y: number; h: number }, b: { x: number; y: number; h: number }, dM: number, lambda: number, samples: number): number {
  let worstNu = -10;
  for (let s = 1; s < samples; s++) {
    const t = s / samples;
    const g = elevationLocal(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    const ray = a.h + (b.h - a.h) * t;
    const d1 = dM * t;
    const d2 = dM - d1;
    const nu = (g - ray) * Math.sqrt((2 * dM) / (lambda * d1 * d2));
    if (nu > worstNu) worstNu = nu;
  }
  return worstNu;
}

export function elevationProfile(from: { lat: number; lon: number }, to: { lat: number; lon: number }): Profile {
  return profileBetween({ ...from, aglM: 0 }, { ...to, aglM: 0 });
}

export { elevation };
