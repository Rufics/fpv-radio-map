import { Injectable, computed, inject, signal } from '@angular/core';
import { Fix, LatLon, Mission, MissionStore, RouteResult, distanceM } from '@rfmap/ui-kit';
import { FpvDataService } from './fpv-data.service';

/** Module id of the altitude corridor module (modules/fpv/altitude). Its options go to mission.modules["fpv.altitude"]. */
export const FPV_ALTITUDE = 'fpv.altitude';

/** modules/fpv/altitude/schema/options.json */
export interface FpvAltitudeOptions {
  stepM?: number;
  clearanceM?: number;
  takeoffM?: number;
  ceilingAglM?: number;
  linkFloor?: boolean;
  fresnelFraction?: number;
  maxAglM?: number;
  /** Heights for waypoints left to the server (route.prepare); default true */
  autoHeight?: boolean;
  /** The climb assistant: the plan's climb against the energy-optimal profile; default true */
  climbAssist?: boolean;
  /** 0 = the drone's measured climb rate, else 3 m/s */
  climbRateMps?: number;
  descentRateMps?: number;
  /** 0 = the drone's cruise airspeed, else 15 m/s */
  speedMps?: number;
  heightStepM?: number;
  fitToleranceM?: number;
}

/** How a height profile does along the route, the same for the plan and every strategy. */
export interface FpvClimbStats {
  /** Total climb from the pilot on, m */
  riseM: number;
  timeS: number;
  /** Lowest height over the safe floor past the take-off zone; negative = under it */
  minClearanceM: number;
  minClearanceAtM: number;
  /** Highest height over the ceiling where the floor is under it; 0 inside */
  overCeilingM: number;
  steepestClimbMps: number;
  steepestClimbAtM: number;
  /** The steepest descent past the take-off zone, as a positive rate. */
  steepestDescentMps: number;
  waypoints: number;
  /** Only with a drone picked */
  energyWh?: number;
}

export type FpvClimbStrategyId = 'energy' | 'over-launch' | 'gradual';

export interface FpvClimbStrategy extends FpvClimbStats {
  id: FpvClimbStrategyId;
  title: string;
  summary: string;
  /** false when the drone cannot follow it past the take-off zone */
  withinClimbRate: boolean;
  addedWaypoints: number;
  /** What its waypoints fly at every corridor sample, MSL */
  mslM: number[];
  /** Its waypoints as a fix, applied through the preview */
  fix: Fix;
}

/** data.climb: the climb assistant. */
export interface FpvClimbData {
  recommended: FpvClimbStrategyId;
  climbRateMps: number;
  descentRateMps: number;
  airspeedMps: number;
  energyEstimated: boolean;
  heightStepM: number;
  fitToleranceM: number;
  plan: FpvClimbStats;
  strategies: FpvClimbStrategy[];
  notes: string[];
  drone?: { id: string; version: number };
}

/** data.corridor: parallel arrays, one entry per sample along the route; heights above mean sea level. */
export interface FpvAltitudeCorridor {
  stepM: number;
  clearanceM: number;
  takeoffM: number;
  ceilingAglM: number;
  linkFloor: boolean;
  fresnelFraction: number;
  /** Distance along the route, m */
  s: number[];
  lat: number[];
  lon: number[];
  leg: number[];
  groundMslM: number[];
  plannedMslM: number[];
  /** The higher of the terrain floor and the link floor */
  floorMslM: number[];
  terrainFloorMslM: number[];
  /** null at the pilot, with the link floor off, or where no height clears the link */
  linkFloorMslM: (number | null)[];
  ceilingMslM: number[];
  /** First Fresnel zone radius half way from the pilot to the sample, at the lowest link frequency */
  fresnelRadiusM: number[];
}

export interface FpvAltitudeWaypoint {
  index: number;
  label?: string;
  s: number;
  groundMslM: number;
  mslM: number;
  aglM: number;
  aboveLaunchM: number;
}

/** RouteResult.modules["fpv.altitude"]: the module's route.analyze `data`, as returned. */
export interface FpvAltitudeData {
  kFactor?: number;
  notes?: string[];
  reference?: 'launch' | 'agl';
  pilot?: { lat: number; lon: number; groundMslM: number; antennaMslM: number };
  corridor?: FpvAltitudeCorridor;
  waypoints?: FpvAltitudeWaypoint[];
  summary?: {
    highestGroundMslM: number;
    highestGroundAtM: number;
    highestPlannedMslM: number;
    highestPlannedAtM: number;
    lowestMarginM: number;
    lowestMarginAtM: number;
  };
  fresnel?: { link: string; radio?: string; frequencyHz: number };
  climb?: FpvClimbData;
}

/** Where the plan is at one sample relative to the corridor. */
export type CorridorState = 'inside' | 'low' | 'ground' | 'high' | 'takeoff';

export function fpvAltitudeData(result: RouteResult | null | undefined): FpvAltitudeData | null {
  return (result?.modules?.[FPV_ALTITUDE] as FpvAltitudeData | undefined) ?? null;
}

export function fpvAltitudeOptions(m: Mission | null | undefined): FpvAltitudeOptions | null {
  return (m?.modules?.[FPV_ALTITUDE] as FpvAltitudeOptions | undefined) ?? null;
}

/** Whether the server sets the heights of waypoints left to it: fpv.altitude is on with autoHeight. */
export function autoHeightsOn(m: Mission | null | undefined): boolean {
  const o = fpvAltitudeOptions(m);
  return !!o && o.autoHeight !== false;
}

/** Pure reading of a corridor: states, interpolation and the cursor point. */
export class Corridor {
  constructor(
    readonly c: FpvAltitudeCorridor,
    private readonly pilot: LatLon,
  ) {}

  get length(): number {
    return this.c.s.at(-1) ?? 0;
  }

  /** Within the take-off zone the floor ramps up from the ground: the floor there is drawn but not judged. */
  state(i: number): CorridorState {
    const planned = this.c.plannedMslM[i];
    if (planned < this.c.groundMslM[i]) return 'ground';
    if (planned > this.c.ceilingMslM[i] + 0.05) return 'high';
    if (planned < this.c.floorMslM[i] - 0.05) return this.nearPilot(i) ? 'takeoff' : 'low';
    return 'inside';
  }

  /** Index of the last sample at or before `s`. */
  indexAt(s: number): number {
    const xs = this.c.s;
    let lo = 0;
    let hi = xs.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (xs[mid] <= s) lo = mid;
      else hi = mid - 1;
    }
    return lo;
  }

  /** A per-sample value at `s`, linear between samples (null where either neighbour is null). */
  valueAt(
    key:
      'groundMslM' | 'plannedMslM' | 'floorMslM' | 'ceilingMslM' | 'fresnelRadiusM' | 'lat' | 'lon',
    s: number,
  ): number {
    return this.profileAt(this.c[key], s);
  }

  /** Any per-sample profile (such as a climb strategy's heights) at `s`, linear between samples. */
  profileAt(v: number[], s: number): number {
    const i = this.indexAt(s);
    const j = Math.min(i + 1, this.c.s.length - 1);
    const span = this.c.s[j] - this.c.s[i];
    const t = span > 0 ? Math.min(1, Math.max(0, (s - this.c.s[i]) / span)) : 0;
    return v[i] + (v[j] - v[i]) * t;
  }

  /** The module's take-off zone: closer to the pilot than takeoffM, along the ground. */
  nearPilot(i: number): boolean {
    return distanceM(this.pilot, { lat: this.c.lat[i], lon: this.c.lon[i] }) < this.c.takeoffM;
  }
}

/**
 * The altitude view: 2D or 3D map, vertical exaggeration, the profile drawer and its cursor, and
 * which corridor layers are drawn. The corridor itself comes from fpv.altitude through /calc/route.
 */
@Injectable({ providedIn: 'root' })
export class AltitudeView {
  private readonly store = inject(MissionStore);
  private readonly data = inject(FpvDataService);

  readonly mode = signal<'2d' | '3d'>('2d');
  /** Vertical exaggeration of terrain and route in 3D; 1 = true scale */
  readonly exaggeration = signal(2);
  /** The safe corridor in the profile */
  readonly corridorShown = signal(true);
  /** The safe corridor in 3D, as dashed floor and ceiling lines; off by default */
  readonly corridor3d = signal(false);
  /** How coverage shows over the 3D terrain; only the 0 dB edge for now */
  readonly coverage3d = signal<'edge'>('edge');
  readonly fresnelShown = signal(false);
  readonly profileOpen = signal(false);
  /** Distance along the route under the profile cursor, m; null = no cursor */
  readonly cursorS = signal<number | null>(null);
  /** The climb strategy drawn over the profile as a dashed ghost; null = none */
  readonly ghost = signal<FpvClimbStrategyId | null>(null);

  /** fpv.altitude is installed and active on this server */
  readonly available = computed(() =>
    this.data.modules().some((m) => m.id === FPV_ALTITUDE && (m.state ?? 'active') === 'active'),
  );
  readonly enabled = computed(() => fpvAltitudeOptions(this.store.mission()) !== null);
  readonly result = computed(() => fpvAltitudeData(this.store.result()));
  readonly corridor = computed(() => {
    const r = this.result();
    const c = r?.corridor;
    return c && r.pilot && c.s.length > 1 ? new Corridor(c, r.pilot) : null;
  });
  /** The climb assistant's strategies, when the module ran it. */
  readonly climb = computed(() => this.result()?.climb ?? null);
  /** The ghost strategy, when the current result still has it. */
  readonly ghostStrategy = computed(() => {
    const id = this.ghost();
    return this.climb()?.strategies.find((s) => s.id === id) ?? null;
  });
  /** The 3D view draws the corridor of the mission it was calculated for, not of an edit since. */
  readonly stale = computed(() => this.store.resultStale());

  setMode(mode: '2d' | '3d'): void {
    this.mode.set(mode);
    if (mode === '3d') this.profileOpen.set(true);
  }

  /** Opts the mission in to fpv.altitude and recalculates. */
  async enable(): Promise<void> {
    this.store.update((m) => ({
      ...m,
      modules: {
        ...m.modules,
        [FPV_ALTITUDE]: (fpvAltitudeOptions(m) ?? {}) as Record<string, unknown>,
      },
    }));
    await this.store.calculate();
  }

  setOptions(patch: FpvAltitudeOptions): void {
    this.store.update((m) => ({
      ...m,
      modules: { ...m.modules, [FPV_ALTITUDE]: { ...fpvAltitudeOptions(m), ...patch } },
    }));
  }
}
