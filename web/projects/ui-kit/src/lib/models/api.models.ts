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
  revision?: number;
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
  /** Set while paused after too many wrong PINs */
  pausedUntil?: string;
  /** Successful opens */
  openCount?: number;
  /** Absent = valid until revoked */
  expiresAt?: string;
}

export interface CreateLinkRequest {
  role: Role;
  /** ^[0-9]{6}$ */
  pin: string;
  label?: string;
  /** Optional; sessions from the link end then too */
  expiresAt?: string;
}

/** GET /links/{token}: public, before the PIN. */
export interface LinkInfo {
  role?: Role;
  missionName?: string;
  expiresAt?: string;
  pausedUntil?: string;
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
  /** Stated vertical error of the DEM */
  uncertaintyM?: number;
  lat?: number;
  lon?: number;
}

export interface Profile {
  source?: string;
  lengthM?: number;
  /** DEM native step (or half the path when shorter) */
  stepM?: number;
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
  minClearancePoint?: ProfilePoint;
}

export interface LinkEnd {
  lat?: number;
  lon?: number;
  /** As entered */
  aglM?: number;
  /** From the DEM */
  groundMslM?: number;
  antennaMslM?: number;
}

export interface LibraryRef {
  kind?: string;
  id?: string;
  version?: number;
  verified?: boolean;
}

/** rf.uncertainty: margin with every stated error at its worst and best. */
export interface UncertaintyBand {
  nominalDb?: number;
  worstDb?: number;
  bestDb?: number;
  verdict?: 'reliable' | 'marginal' | 'blocked-may-work' | 'blocked';
  source?: string;
}

export interface UncertaintyNote {
  what?: string;
  detail?: string;
}

export interface LinkResult {
  kind?: LinkKind;
  frequencyHz?: number;
  txPowerDbm?: number;
  /** Geodesic ground distance */
  distanceM?: number;
  rayLengthM?: number;
  terms?: BudgetTerm[];
  receivedDbm?: number;
  sensitivityDbm?: number;
  requiredMarginDb?: number;
  /** sensitivity + required margin */
  requiredDbm?: number;
  /** receivedDbm − requiredDbm */
  marginDb?: number;
  snrDb?: number;
  reachable?: boolean;
  /** Analog video only */
  analogGrade?: 'clear' | 'noisy' | 'lost';
  latencyMs?: number;
  diffraction?: Diffraction;
  band?: UncertaintyBand;
  ends?: { ground?: LinkEnd; air?: LinkEnd; transmitter?: 'ground' | 'air' };
  earth?: { effectiveEarthFactor?: number; source?: string };
  weather?: WeatherPoint;
  forecastTier?: ResultForecastTier;
  library?: LibraryRef[];
  /** In a leg's worst result, where along the leg it was taken */
  sample?: { lat?: number; lon?: number; aglM?: number; alongLegM?: number };
  /** DEM, forecast, datasheet and library notes. Always present. */
  uncertainty?: UncertaintyNote[];
  moduleFailures?: ModuleFailure[];
  inputHash?: string;
  /** Ordered from the transmitter */
  profile?: Profile;
  /** Model and version, e.g. rf-core 0.1.0 / Bullington exact */
  model?: string;
}

/** A span without DEM data along a path. */
export interface MissingSpan {
  fromM?: number;
  toM?: number;
}

/** Where terrain data is missing: [west, south, east, north] of the DEM samples without a value. */
export interface MissingDataArea {
  bbox?: [number, number, number, number];
  /** DEM name */
  source?: string;
}

/** 422 body of /calc/path and /elevation/profile. Terrain gaps are never filled. */
export interface MissingData {
  status?: 'missing-data';
  message?: string;
  missingData?: MissingDataArea;
  missing?: MissingSpan[];
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
  link?: LinkKind;
  /** Module reasons only: the leg it was attached to. Without it the reason is route-wide (RouteResult.reasons). */
  leg?: number;
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
  /** Up to 20 drone positions whose path from the pilot lacks DEM data; spans are along that path */
  missing?: { alongLegM?: number; lat?: number; lon?: number; missing?: MissingSpan[] }[];
  /** Union over the leg */
  missingData?: MissingDataArea;
  samples?: { stepM?: number; total?: number; evaluated?: number; missingData?: number; overPilot?: number };
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
/** none = beyond the forecast (k = 4/3); off = rf.weather not opted in by the mission or disabled */
export type ResultForecastTier = ForecastTier | 'off';

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
  precipitationMmPerHour?: number;
  surfaceElevationM?: number;
  surfaceRefractivityN?: number;
  requestedUtc?: string;
  /** Set when the requested time is beyond the forecast (tier none) */
  forecastUntilUtc?: string;
  stale?: boolean;
  notes?: UncertaintyNote[];
}

export interface ModuleFailure {
  module?: string;
  error?: string;
}

export interface RouteResult {
  /** Only when the body equals the stored mission */
  missionRevision?: number;
  inputHash?: string;
  /** Time the forecast was taken for */
  timeUtc?: string;
  legs?: LegResult[];
  /** Route-wide reasons from modules (reasons without a leg) */
  reasons?: Reason[];
  battery?: BatteryResult;
  /** At the pilot, when rf.weather is opted in */
  weather?: WeatherPoint;
  forecastTier?: ResultForecastTier;
  library?: LibraryRef[];
  /** Per-module output keyed by module id: the module's route.analyze `data` as returned */
  modules?: Record<string, unknown>;
  moduleFailures?: ModuleFailure[];
}

export type JobState = 'queued' | 'running' | 'done' | 'failed' | 'cancelled';
export type JobPartState = 'waiting' | 'running' | 'done' | 'cached' | 'failed';

export interface JobPart {
  /** For display, e.g. "Coverage grid, control link" */
  name?: string;
  link?: LinkKind;
  state?: JobPartState;
  progress?: number;
  error?: string;
}

export interface Job {
  jobId?: string;
  state?: JobState;
  createdAt?: string;
  parts?: JobPart[];
}

/**
 * Regular grid in latitude/longitude. Cell (row r, col c) spans
 * lat [origin.lat − (r+1)·cellLatDeg, origin.lat − r·cellLatDeg] and
 * lon [origin.lon + c·cellLonDeg, origin.lon + (c+1)·cellLonDeg]; the margin is taken at its centre.
 */
export interface CoverageGrid {
  link?: LinkKind;
  /** North-west corner of the grid */
  origin?: { lat?: number; lon?: number };
  pilot?: { lat?: number; lon?: number };
  /** Requested cell size; the grid uses the equivalent degrees at the pilot */
  cellM?: number;
  cellLatDeg?: number;
  cellLonDeg?: number;
  cols?: number;
  rows?: number;
  radiusM?: number;
  droneAglM?: number;
  /** [west, south, east, north] */
  bbox?: [number, number, number, number];
  /** Row-major, north to south; null = beyond the radius, at the pilot, or no DEM data */
  marginDb?: (number | null)[];
  /** Same layout; worst edge of the uncertainty band */
  marginWorstDb?: (number | null)[];
  missingDataCells?: number;
  /** GeoJSON MultiPolygon of cell edges around the cells with margin ≥ 0 dB (exact, not smoothed) */
  limitPolygon?: Polygon | MultiPolygon | Feature | Record<string, unknown>;
  library?: LibraryRef[];
  timeUtc?: string;
  weather?: WeatherPoint;
  forecastTier?: ResultForecastTier;
  inputHash?: string;
  modules?: Record<string, unknown>;
  reasons?: Reason[];
  moduleFailures?: ModuleFailure[];
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
  description?: string;
  /** Built-in RF extension; can be disabled, not removed */
  builtIn?: boolean;
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
  /** Server-wide options set by the admin */
  settings?: Record<string, unknown>;
  state?: ModuleState;
  stateDetail?: string;
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

export type CacheTierId = 'dem-blocks' | 'results-memory' | 'results-table';

export interface CacheTier {
  id: CacheTierId;
  label?: string;
  limitBytes?: number;
  /** results-table only */
  limitRows?: number;
  usedBytes?: number;
  entries?: number;
  hits?: number;
  misses?: number;
  evictions?: number;
  hitRate?: number;
}

/** PUT /admin/cache item */
export interface CacheLimit {
  id: CacheTierId;
  limitBytes?: number;
  limitRows?: number;
}

export interface TrustedKey {
  id?: string;
  label?: string;
  key?: string;
  builtIn?: boolean;
  firstParty?: boolean;
}

/** One version in GET /library/{kind}/{id}/history */
export interface LibraryHistoryEntry {
  version?: number;
  verified?: boolean;
  item?: Record<string, unknown>;
  changedBy?: string;
  changedAt?: string;
}

/**
 * GET /config/map. The contract gives `pmtiles` (+ `sourceLayer` for contours); the other fields
 * are client extensions used by the dev mock (inline GeoJSON, XYZ templates).
 */
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
  /** From the tiles-builder manifest.json next to the DEM */
  attribution?: string[];
  /** [lon, lat], the DEM centre */
  center?: [number, number];
  zoom?: number;
}
