import { Injectable, computed, inject, signal } from '@angular/core';
import {
  Antenna,
  Battery,
  Drone,
  ForecastHorizon,
  ForecastTier,
  MapSourcesConfig,
  ModuleInfo,
  RadioProfile,
  RfmapApiClient,
} from '@rfmap/ui-kit';
import { environment } from '../../environments/environment';

/** Reference library, forecast horizon, map config and module list — loaded once. */
@Injectable({ providedIn: 'root' })
export class FpvDataService {
  private readonly api = inject(RfmapApiClient);

  readonly radios = signal<RadioProfile[]>([]);
  readonly antennas = signal<Antenna[]>([]);
  readonly batteries = signal<Battery[]>([]);
  readonly drones = signal<Drone[]>([]);
  readonly horizon = signal<ForecastHorizon | null>(null);
  readonly mapConfig = signal<MapSourcesConfig | null>(null);
  readonly modules = signal<ModuleInfo[]>([]);
  readonly loaded = signal(false);

  private loading?: Promise<void>;

  load(): Promise<void> {
    this.loading ??= this.doLoad();
    return this.loading;
  }

  private async doLoad(): Promise<void> {
    const safe = async <T>(p: Promise<T>, fallback: T): Promise<T> => {
      try {
        return await p;
      } catch {
        return fallback;
      }
    };
    const [radios, antennas, batteries, drones, horizon, config, modules] = await Promise.all([
      safe(this.api.library<RadioProfile>('radio-profiles'), []),
      safe(this.api.library<Antenna>('antennas'), []),
      safe(this.api.library<Battery>('batteries'), []),
      safe(this.api.library<Drone>('drones'), []),
      safe(this.api.weatherHorizon(), null),
      safe(this.api.mapConfig(), environment.mapFallback as MapSourcesConfig),
      safe(this.api.modules(), []),
    ]);
    this.radios.set(radios);
    this.antennas.set(antennas);
    this.batteries.set(batteries);
    this.drones.set(drones);
    this.horizon.set(horizon);
    this.mapConfig.set(config);
    this.modules.set(modules);
    this.loaded.set(true);
  }

  async reloadLibrary(): Promise<void> {
    this.radios.set(await this.api.library<RadioProfile>('radio-profiles'));
    this.antennas.set(await this.api.library<Antenna>('antennas'));
    this.batteries.set(await this.api.library<Battery>('batteries'));
    this.drones.set(await this.api.library<Drone>('drones'));
  }

  radio(id: string | undefined): RadioProfile | undefined {
    return this.radios().find((r) => r.id === id);
  }
  antenna(id: string | undefined): Antenna | undefined {
    return this.antennas().find((a) => a.id === id);
  }
  battery(id: string | undefined): Battery | undefined {
    return this.batteries().find((b) => b.id === id);
  }
  drone(id: string | undefined): Drone | undefined {
    return this.drones().find((d) => d.id === id);
  }

  /** Forecast tier for a time, from the run the server holds. */
  tierFor(timeUtc: string | null): ForecastTier {
    const h = this.horizon();
    if (!h?.hourlyUntilUtc || !h.longRangeUntilUtc) return 'none';
    const t = timeUtc ? new Date(timeUtc).getTime() : Date.now();
    if (t <= new Date(h.hourlyUntilUtc).getTime()) return 'hourly';
    if (t <= new Date(h.longRangeUntilUtc).getTime()) return 'long-range';
    return 'none';
  }

  readonly moduleTitles = computed(() => new Map(this.modules().map((m) => [m.id ?? '', m.title ?? m.id ?? ''])));
}

/**
 * Battery range shown before the server answered: usable energy over cruise power at cruise speed,
 * from the library entries the mission uses. The calculated value replaces it.
 */
export function estimateRangeM(battery: Battery | undefined, drone: Drone | undefined): number | null {
  if (!battery || !drone) return null;
  const usableWh = battery.cellsSeries * battery.nominalCellVoltage * battery.capacityAh * battery.usableFraction;
  const hours = usableWh / drone.cruisePowerW;
  return hours * 3600 * drone.cruiseAirspeedMps;
}
