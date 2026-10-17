/** Hand-written types for schemas/library/*.schema.json (draft-07). */

export type LibraryKind = 'radio-profiles' | 'antennas' | 'batteries' | 'drones';

export const LIBRARY_KINDS: { kind: LibraryKind; schemaFile: string; title: string }[] = [
  { kind: 'radio-profiles', schemaFile: 'radio-profile.schema.json', title: 'Radio profiles' },
  { kind: 'antennas', schemaFile: 'antenna.schema.json', title: 'Antennas' },
  { kind: 'batteries', schemaFile: 'battery.schema.json', title: 'Batteries' },
  { kind: 'drones', schemaFile: 'drone.schema.json', title: 'Airframes' },
];

export interface Source {
  kind: 'datasheet' | 'measurement' | 'standard' | 'derived';
  reference: string;
  url?: string;
  retrieved?: string;
}

export interface EntryMeta {
  id: string;
  name: string;
  vendor?: string;
  version: number;
  verified: boolean;
  notes?: string;
}

export type Sensitivity =
  | { method: 'computed'; bandwidthHz: number; noiseFigureDb: number; requiredSnrDb: number; source: Source }
  | { method: 'datasheet'; dBm: number; source: Source };

export interface RadioProfile extends EntryMeta {
  kind: 'control' | 'video-digital' | 'video-analog';
  frequencyHz: number;
  txPowerDbm: number[];
  sensitivity: Sensitivity;
  latencyMs: { typical: number; max?: number; source?: Source };
  analogGrades?: { label: 'clear' | 'noisy' | 'lost'; requiredSnrDb: number }[];
  extraNoiseDb?: number;
}

export interface Antenna extends EntryMeta {
  type: 'dipole' | 'monopole' | 'patch' | 'helical' | 'yagi' | 'omni-collinear' | 'other';
  gainDbi: number;
  polarization: 'linear-vertical' | 'linear-horizontal' | 'rhcp' | 'lhcp';
  bandHz: [number, number];
  beamwidthDeg?: { horizontal?: number; vertical?: number };
  feederLossDb?: number;
  source: Source;
}

export interface Battery extends EntryMeta {
  chemistry: 'lipo' | 'li-ion' | 'lihv';
  cellsSeries: number;
  capacityAh: number;
  nominalCellVoltage: number;
  usableFraction: number;
  temperatureDerating?: { celsius: number; factor: number }[];
  source: Source;
}

export interface Drone extends EntryMeta {
  massKg: number;
  cruiseAirspeedMps: number;
  cruisePowerW: number;
  powerDensityExponent?: number;
  climb: { method: 'potential-energy' | 'measured'; efficiency?: number; climbPowerW?: number; climbRateMps?: number };
  defaultRadios?: { control?: string; video?: string };
  source: Source;
}

export type LibraryEntry = RadioProfile | Antenna | Battery | Drone;
