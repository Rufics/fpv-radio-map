/**
 * Hand-written types for schemas/mission.schema.json (draft-07).
 * Keep in sync with the schema; `npm run sync-schemas` copies the schema next to this file
 * and the spec `mission.models.spec.ts` validates the reference mission against it with ajv.
 */

/** What waypoint heights are measured from: the ground at the pilot (a barometric altitude from take-off) or the ground under the drone. */
export type AltitudeRef = 'launch' | 'agl';

/** #/$defs/point and #/$defs/pilot */
export interface MissionPoint {
  lat: number;
  lon: number;
  /** Height above ground; MSL is computed from the DEM. For the pilot, the antenna height. */
  aglM: number;
  label?: string;
}

/** #/$defs/waypoint: `altM` when the mission is launch-referenced, `aglM` otherwise. */
export interface MissionWaypoint {
  lat: number;
  lon: number;
  /** Height above the ground under the drone (altitudeRef agl). */
  aglM?: number;
  /** Height above the ground at the pilot (altitudeRef launch); negative is below the launch point. */
  altM?: number;
  label?: string;
}

/** #/$defs/linkSetup */
export interface LinkSetup {
  radio: string;
  txPowerDbm: number;
  groundAntenna: string;
  airAntenna: string;
  requiredMarginDb: number;
}

export type LinkKind = 'control' | 'video';
export type EvaluateMode = 'both' | 'control' | 'video';

export interface MissionTime {
  mode: 'now' | 'planned';
  utc?: string;
}

export interface CoverageSettings {
  radiusM?: number;
  cellM?: number;
  droneAglM?: number;
}

export interface Mission {
  schema: 'mission/v1';
  name: string;
  time: MissionTime;
  /** Without it a mission is 'agl', the original format. */
  altitudeRef?: AltitudeRef;
  pilot: MissionPoint;
  /** Route in flight order; the last point is the target. */
  waypoints: MissionWaypoint[];
  returnToPilot?: boolean;
  links: { control?: LinkSetup; video?: LinkSetup };
  evaluate: EvaluateMode;
  drone?: string;
  battery?: string;
  modules?: Record<string, Record<string, unknown>>;
  coverage?: CoverageSettings;
}

export function linksEvaluated(mode: EvaluateMode): LinkKind[] {
  return mode === 'both' ? ['control', 'video'] : [mode];
}

/**
 * Reads and writes waypoint heights in the mission's altitude reference. Between two waypoints the
 * server interpolates the height linearly in that reference: above the launch point the drone flies a
 * straight line in altitude; above ground it follows the terrain.
 */
export class MissionAltitude {
  /** Height new waypoints get. */
  static readonly defaultHeightM = 120;

  static ref(m: Pick<Mission, 'altitudeRef'>): AltitudeRef {
    return m.altitudeRef ?? 'agl';
  }

  static field(m: Pick<Mission, 'altitudeRef'>): 'altM' | 'aglM' {
    return MissionAltitude.ref(m) === 'launch' ? 'altM' : 'aglM';
  }

  /** The waypoint's height in the mission's reference (0 when the field is missing). */
  static height(m: Pick<Mission, 'altitudeRef'>, w: MissionWaypoint): number {
    return w[MissionAltitude.field(m)] ?? 0;
  }

  /** The waypoint with its height set in the mission's reference, and the other reference's field removed. */
  static withHeight<T extends MissionWaypoint>(m: Pick<Mission, 'altitudeRef'>, w: T, heightM: number): T {
    const rest: MissionWaypoint = { ...w };
    delete rest.aglM;
    delete rest.altM;
    return { ...rest, [MissionAltitude.field(m)]: heightM } as T;
  }

  /** Short unit for a height in the mission's reference, as shown next to numbers. */
  static unit(m: Pick<Mission, 'altitudeRef'>): string {
    return MissionAltitude.ref(m) === 'launch' ? 'm above launch' : 'm AGL';
  }

  /** What the height is measured from, for labels and titles. */
  static basis(m: Pick<Mission, 'altitudeRef'>): string {
    return MissionAltitude.ref(m) === 'launch' ? 'above the launch point' : 'above ground';
  }

  /**
   * The mission in another reference. The numbers are kept: the client has no terrain to convert them
   * with, and over flat ground they mean the same. The user reviews them after switching.
   */
  static withRef(m: Mission, ref: AltitudeRef): Mission {
    if (MissionAltitude.ref(m) === ref) return m;
    const target = { altitudeRef: ref };
    return { ...m, altitudeRef: ref, waypoints: m.waypoints.map((w) => MissionAltitude.withHeight(target, w, MissionAltitude.height(m, w))) };
  }
}
