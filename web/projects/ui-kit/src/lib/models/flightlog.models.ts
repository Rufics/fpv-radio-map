/**
 * Flight log replay (flightview/v1): what GET /missions/{id}/flightlogs/{logId} and
 * POST /flightlogs/decode answer. The server builds it with rf-flightlog from the normalized
 * flightlog/v1 document a decoder module (fpv.blackbox for Betaflight Blackbox) produced, so the
 * shape is the same for every log format.
 */

export type FlightBand = 'DISARMED' | 'ACRO' | 'ANGLE' | 'HORIZON' | 'ALTHOLD' | 'POSHOLD' | 'GPS_RESCUE' | 'FAILSAFE';

export type FlightSeverity = 'info' | 'warn' | 'blocked';

/** Names of the timeline channels; any may be missing when the log does not have it. */
export type FlightChannel =
  | 'lat'
  | 'lon'
  | 'altM'
  | 'altMslM'
  | 'homeDistM'
  | 'distanceM'
  | 'speedMps'
  | 'climbMps'
  | 'voltageV'
  | 'cellV'
  | 'currentA'
  | 'usedMah'
  | 'usedWh'
  | 'rssiPct'
  | 'linkQualityPct'
  | 'rssiDbm'
  | 'throttlePct'
  | 'baroAltM'
  | 'sats'
  | 'gpsFix'
  | 'armed';

export interface FlightTimeline {
  hz: number;
  /** Seconds from the start of the log */
  t: number[];
  channels: Partial<Record<FlightChannel, (number | null)[]>>;
}

export interface FlightModeBand {
  from: number;
  to: number;
  band: FlightBand;
}

export interface FlightStoryEvent {
  t: number;
  type: string;
  severity: FlightSeverity;
  title: string;
  detail?: string;
  lat?: number;
  lon?: number;
}

export interface FlightSummary {
  durationS: number;
  armedS: number;
  cells?: number;
  distanceM?: number;
  maxHomeDistM?: number;
  maxAltM?: number;
  maxSpeedMps?: number;
  maxCurrentA?: number;
  usedMah?: number;
  usedWh?: number;
  startVoltageV?: number;
  minVoltageV?: number;
  minCellV?: number;
  minLinkPct?: number;
  linkChannel?: 'linkQualityPct' | 'rssiPct';
  disarmReason?: string;
}

export interface FlightLogNote {
  code: string;
  severity: FlightSeverity;
  message: string;
  fix?: string;
}

export interface FlightFirmware {
  name?: string;
  version?: string;
  revision?: string;
  target?: string;
  board?: string;
  supported?: boolean;
}

/** A logbook row: GET /missions/{id}/flightlogs, and each flight of an import. */
export interface FlightLogRow {
  id?: string;
  index: number;
  status: 'ok' | 'partial' | 'header-only' | 'unsupported';
  durationS: number;
  firmware: FlightFirmware;
  craft?: string;
  startedAt?: string;
  summary: FlightSummary;
  bands: FlightModeBand[];
  /** Titles of the warn and blocked story events */
  warnings: string[];
  fileName?: string;
  decoder?: string;
  importedAt?: string;
}

export interface PlanFactRow {
  metric: 'distance' | 'energy' | 'whPerKm' | 'weakestLink' | 'maxOffRoute';
  label: string;
  unit: string;
  plan: number | null;
  fact: number | null;
  deltaPct?: number;
}

export interface PlanFactLeg {
  index: number;
  from: string;
  to: string;
  lengthM: number;
  controlMarginDb: number | null;
  videoMarginDb: number | null;
  flownS: number;
  minLinkPct: number | null;
  maxOffRouteM: number;
}

export interface PlanFact {
  /** The mission had a route result to compare with (margins, energy) */
  evaluated: boolean;
  rows: PlanFactRow[];
  legs: PlanFactLeg[];
  findings: { severity: FlightSeverity; text: string }[];
  /** Distance from the planned route per timeline sample, while armed */
  offRouteM: (number | null)[];
  furthest?: { t: number; lat: number; lon: number; distanceM: number; leg: number };
}

/** The whole replay of one flight. */
export interface FlightView extends FlightLogRow {
  schema: 'flightview/v1';
  channels: Record<string, boolean>;
  notes: FlightLogNote[];
  radio?: { protocol?: string };
  homes: { t: number; lat: number; lon: number }[];
  story: FlightStoryEvent[];
  timeline: FlightTimeline;
  /** Only for a flight stored in a mission */
  plan?: PlanFact;
}

/** POST /flightlogs/decode */
export interface FlightLogPreview {
  decoder: string;
  source: { decoder: string; decoderVersion?: string; format?: string; fileName?: string; bytes?: number };
  flights: FlightView[];
}

/** POST /missions/{id}/flightlogs */
export interface FlightLogImport {
  decoder: string;
  fileName: string;
  flights: FlightLogRow[];
}
