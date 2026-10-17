/**
 * Hand-written types for the components in docs/api/openapi.yaml.
 * Field names and enums follow the contract exactly. Anything the client needs that the
 * contract does not define is marked `@gap` and listed in web/API-NOTES.md.
 */
import type { Feature, FeatureCollection, MultiPolygon, Polygon } from 'geojson';
import { LinkKind, Mission, MissionPoint, LinkSetup } from './mission.models';

export type Role = 'edit' | 'view';

export interface CreateMissionResponse {
  missionId: string;
  /** Shown once. Needed to create or revoke links. */
  ownerKey: string;
  session: string;
}

export interface GetMissionResponse {
  mission: Mission;
  role: Role;
  revision: number;
  lastResult?: RouteResult;
}

export interface ShareLink {
  linkId: string;
  role: Role;
  label?: string;
  createdAt?: string;
  lastOpenedAt?: string;
  revoked?: boolean;
}

export interface CreateLinkRequest {
  role: Role;
  /** ^[0-9]{6}$ */
  pin: string;
  label?: string;
}

export type CreatedShareLink = ShareLink & { token?: string };

export interface OpenLinkResponse {
  session: string;
  missionId: string;
  role: Role;
  missionName: string;
}

export interface BudgetTerm {
  id: string;
  label: string;
  /** Positive = loss, negative = gain */
  valueDb: number;
  source: string;
}

export interface ProfilePoint {
  distanceM: number;
  groundMslM: number;
  obstacleM?: number;
  lat?: number;
  lon?: number;
}

export interface Profile {
  source?: string;
  lengthM?: number;
  points: ProfilePoint[];
}

export interface Diffraction {
  lineOfSight?: boolean;
  nu?: number;
  edgeHeightM?: number;
  d1M?: number;
  d2M?: number;
  lossDb?: number;
  minFresnelClearance?: number;
  firstFresnelRadiusM?: number;
  dominant?: ProfilePoint;
}

export interface UncertaintyNote {
  what?: string;
  detail?: string;
}

export interface LinkResult {
  kind?: LinkKind;
  frequencyHz?: number;
  rayLengthM?: number;
  terms?: BudgetTerm[];
  receivedDbm?: number;
  requiredDbm?: number;
  marginDb?: number;
  reachable?: boolean;
  /** Analog video only */
  analogGrade?: 'clear' | 'noisy' | 'lost';
  latencyMs?: number;
  diffraction?: Diffraction;
  uncertainty?: UncertaintyNote[];
  profile?: Profile;
  /** Model and version, e.g. rf-core 0.1.0 / Bullington exact */
  model?: string;
}

export interface MissingData {
  message?: string;
  missing?: { fromM?: number; toM?: number }[];
}

export interface FixPreviewRow {
  label?: string;
  now?: string;
  after?: string;
  /** @gap Not in the contract: whether "after" is better, worse or neutral (colour + mark in the preview). */
  trend?: 'better' | 'worse' | 'caution' | 'neutral';
}

/** A suggested change. The client shows a preview (before/after) and applies only on confirmation. */
export interface Fix {
  id?: string;
  title?: string;
  /** JSON merge patch (RFC 7386) to the mission */
  patch?: Record<string, unknown>;
  preview?: FixPreviewRow[];
  /** @gap Not in the contract: one-paragraph explanation shown under the preview table. */
  note?: string;
}

export type Severity = 'info' | 'warn' | 'blocked';

/** One line of the "why" card, contributed by the core or a module. */
export interface Reason {
  module?: string;
  severity?: Severity;
  text?: string;
  fix?: Fix;
}

export type LegStatus = 'ok' | 'warn' | 'blocked' | 'missing-data';

export interface LegResult {
  index?: number;
  from?: string;
  to?: string;
  lengthM?: number;
  worst?: { control?: LinkResult; video?: LinkResult };
  status?: LegStatus;
  reasons?: Reason[];
}

export interface TurnBack {
  distanceAlongRouteM?: number;
  returnM?: number;
  lat?: number;
  lon?: number;
}

export interface BatteryResult {
  energyWh?: number;
  usableWh?: number;
  neededWh?: number;
  rangeM?: number;
  neededM?: number;
  shortfallM?: number;
  turnBack?: TurnBack;
}

export type ForecastTier = 'hourly' | 'long-range' | 'none';

export interface WeatherPoint {
  model?: string;
  runUtc?: string;
  validUtc?: string;
  tier?: ForecastTier;
  temperatureC?: number;
  pressureHpa?: number;
  relativeHumidity?: number;
  windSpeedMps?: number;
  windFromDeg?: number;
  refractivityGradientPerKm?: number;
  effectiveEarthFactor?: number;
  airDensityKgM3?: number;
  stale?: boolean;
}

export interface ModuleFailure {
  module?: string;
  error?: string;
}

export interface RouteResult {
  missionRevision?: number;
  inputHash?: string;
  legs?: LegResult[];
  battery?: BatteryResult;
  weather?: WeatherPoint;
  /** Per-module output keyed by module id */
  modules?: Record<string, unknown>;
  moduleFailures?: ModuleFailure[];
}

export type JobState = 'queued' | 'running' | 'done' | 'failed' | 'cancelled';
export type JobPartState = 'waiting' | 'running' | 'done' | 'cached' | 'failed';

export interface JobPart {
  name?: string;
  state?: JobPartState;
  progress?: number;
}

export interface Job {
  jobId?: string;
  state?: JobState;
  parts?: JobPart[];
}

export interface CoverageGrid {
  origin?: { lat?: number; lon?: number };
  cellM?: number;
  cols?: number;
  rows?: number;
  /** [west, south, east, north] — @gap: order not stated in the contract, assumed GeoJSON order */
  bbox?: [number, number, number, number];
  /** Row-major, north to south; null = no data */
  marginDb?: (number | null)[];
  /** GeoJSON polygon of the 0 dB margin line */
  limitPolygon?: Polygon | MultiPolygon | Feature | Record<string, unknown>;
}

export interface ForecastHorizon {
  runUtc?: string;
  hourlyUntilUtc?: string;
  longRangeUntilUtc?: string;
}

export type ModuleLifecycle = 'alpha' | 'beta' | 'stable' | 'deprecated';
export type ModuleState = 'active' | 'disabled' | 'missing-dependency' | 'failed';

export interface ModuleInfo {
  id?: string;
  version?: string;
  title?: string;
  lifecycle?: ModuleLifecycle;
  deprecation?: { since?: string; removedIn?: string; replacement?: string; message?: string };
  requires?: string[];
  optional?: string[];
  provides?: string[];
  signedBy?: string;
  isolated?: boolean;
  ui?: { bundle?: string; slots?: string[] };
  /** JSON Schema of the module options */
  options?: Record<string, unknown>;
  state?: ModuleState;
}

export interface ValidationCheck {
  name?: string;
  ok?: boolean;
  detail?: string;
}

export interface ValidationReport {
  ok?: boolean;
  checks?: ValidationCheck[];
}

export interface CalcPathRequest {
  from: MissionPoint;
  to: MissionPoint;
  kind: LinkKind;
  link: LinkSetup;
  timeUtc?: string;
  modules?: Record<string, unknown>;
}

/** @gap GET /admin/cache has no schema in the contract. Shape assumed by the client. */
export interface CacheTier {
  id: string;
  label: string;
  limitBytes: number;
  usedBytes: number;
  hitRate?: number;
}

/** @gap No config endpoint in the contract. Assumed GET /config/map; see API-NOTES.md. */
export interface TileSourceRef {
  /** pmtiles archive URL (served by our server), used through the pmtiles:// protocol */
  pmtiles?: string;
  /** XYZ tile URL templates */
  tiles?: string[];
  /** GeoJSON URL or inline object (dev mock only) */
  geojson?: string | FeatureCollection;
  /** vector source-layer name for contours */
  sourceLayer?: string;
  encoding?: 'terrarium' | 'mapbox';
  tileSize?: number;
  maxzoom?: number;
  attribution?: string;
}

export interface MapSourcesConfig {
  basemap?: TileSourceRef;
  satellite?: TileSourceRef;
  /** Pre-rendered hillshade raster (tools/tiles-builder hillshade.pmtiles) */
  hillshade?: TileSourceRef;
  /** Elevation raster-dem (terrarium/mapbox); used for hillshade when no `hillshade` raster is given */
  dem?: TileSourceRef;
  contours?: TileSourceRef;
  /** Templates with {layer}, {time}, {z}, {x}, {y}; layer = wind | precipitation | temperature */
  weather?: TileSourceRef;
  glyphs?: string;
  attribution?: string[];
  center?: [number, number];
  zoom?: number;
}
