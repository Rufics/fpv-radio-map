/**
 * Hand-written types for schemas/mission.schema.json (draft-07).
 * Keep in sync with the schema; `npm run sync-schemas` copies the schema next to this file
 * and the spec `mission.models.spec.ts` validates the reference mission against it with ajv.
 */

/** #/$defs/point */
export interface MissionPoint {
  lat: number;
  lon: number;
  /** Height above ground; MSL is computed from the DEM. */
  aglM: number;
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
  pilot: MissionPoint;
  /** Route in flight order; the last point is the target. */
  waypoints: MissionPoint[];
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
