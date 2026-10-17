import { Injectable, computed, inject, signal } from '@angular/core';
import { ApiError, CoverageGrid, LinkKind, Mission, MissionStore, RfmapApiClient, RouteResult, applyMergePatch, subtractGrids } from '@rfmap/ui-kit';
import { FpvDataService } from './fpv-data.service';

/** Module id of the FPV core module (modules/fpv/core). Its options go to mission.modules["fpv.core"]. */
export const FPV_CORE = 'fpv.core';

/** modules/fpv/core/schema/options.json */
export interface FpvCoreOptions {
  compare?: { name?: string; patch: Record<string, unknown> };
  stepM?: number;
  export?: ('kml' | 'gpx')[];
  exportStepM?: number;
  kmlAltitude?: 'relativeToGround' | 'absolute';
}

export interface FpvComparePoint {
  lat: number;
  lon: number;
  aMarginDb: number;
  bMarginDb: number;
  deltaDb: number;
}

export interface FpvCompareLink {
  a?: { worstMarginDb?: number; sample?: number };
  b?: { worstMarginDb?: number; sample?: number };
  deltaWorstDb?: number;
  /** Only when the two scenarios share the route geometry (aligned) */
  points?: FpvComparePoint[];
  pointsBetter?: number;
  pointsWorse?: number;
}

export interface FpvCompareData {
  name?: string;
  aligned?: boolean;
  stepM?: number;
  mission?: Mission;
  links?: Partial<Record<LinkKind, FpvCompareLink>>;
}

export interface FpvExportFile {
  mediaType: string;
  content: string;
}

/** RouteResult.modules["fpv.core"] = { data, reasons } */
export interface FpvCoreData {
  compare?: FpvCompareData;
  export?: Partial<Record<'kml' | 'gpx', FpvExportFile>>;
}

export function fpvCoreData(result: RouteResult | null | undefined): FpvCoreData | null {
  const out = result?.modules?.[FPV_CORE] as { data?: FpvCoreData } | undefined;
  return out?.data ?? null;
}

export function fpvCoreOptions(m: Mission | null | undefined): FpvCoreOptions {
  return (m?.modules?.[FPV_CORE] as FpvCoreOptions | undefined) ?? {};
}

/**
 * Feature 13, compare two scenarios: B = A with a JSON merge patch.
 * The route comparison comes from fpv.core through /calc/route; the coverage difference map runs
 * a second coverage job for B and subtracts the grids in the browser.
 */
@Injectable({ providedIn: 'root' })
export class CompareService {
  private readonly store = inject(MissionStore);
  private readonly api = inject(RfmapApiClient);
  private readonly data = inject(FpvDataService);

  readonly link = signal<LinkKind>('video');
  readonly diffState = signal<'idle' | 'running' | 'done' | 'failed'>('idle');
  readonly diffError = signal<string | null>(null);
  readonly delta = signal<CoverageGrid | null>(null);
  private run = 0;

  /** fpv.core is installed and active on this server */
  readonly available = computed(() => this.data.modules().some((m) => m.id === FPV_CORE && (m.state ?? 'active') === 'active'));
  readonly options = computed(() => fpvCoreOptions(this.store.mission()).compare ?? null);
  readonly result = computed(() => fpvCoreData(this.store.result())?.compare ?? null);
  /** Scenario B as a whole mission */
  readonly missionB = computed(() => {
    const m = this.store.mission();
    const o = this.options();
    if (!m || !o) return null;
    const b = applyMergePatch(structuredClone(m), o.patch);
    if (b.modules?.[FPV_CORE]) {
      const { compare: _drop, ...rest } = b.modules[FPV_CORE] as FpvCoreOptions;
      b.modules = { ...b.modules, [FPV_CORE]: rest as Record<string, unknown> };
    }
    return b;
  });

  /** Set scenario B and calculate both. */
  async compare(name: string, patch: Record<string, unknown>): Promise<void> {
    this.clearDiff();
    this.store.update((m) => ({ ...m, modules: { ...(m.modules ?? {}), [FPV_CORE]: { ...fpvCoreOptions(m), compare: { name: name || 'B', patch } } } }));
    await this.store.calculate();
  }

  /** Drop scenario B from the mission. */
  remove(): void {
    this.clearDiff();
    this.store.update((m) => {
      const { compare: _drop, ...rest } = fpvCoreOptions(m);
      const modules = { ...(m.modules ?? {}) };
      if (Object.keys(rest).length) modules[FPV_CORE] = rest as Record<string, unknown>;
      else delete modules[FPV_CORE];
      return { ...m, modules };
    });
  }

  clearDiff(): void {
    this.run++;
    this.delta.set(null);
    this.diffState.set('idle');
    this.diffError.set(null);
  }

  /** Coverage difference map for one link: second coverage job for B, then B − A per cell. */
  async coverageDiff(link: LinkKind): Promise<void> {
    const run = ++this.run;
    this.link.set(link);
    this.delta.set(null);
    this.diffError.set(null);
    const a = this.store.coverage()[link];
    const b = this.missionB();
    if (!a) {
      this.diffState.set('failed');
      this.diffError.set('Calculate the coverage of this mission first.');
      return;
    }
    if (!b) return;
    this.diffState.set('running');
    try {
      const id = this.store.missionId();
      let job = await this.api.startCoverage(id, { ...b, evaluate: link });
      while (job.state === 'queued' || job.state === 'running') {
        await new Promise((r) => setTimeout(r, 400));
        if (run !== this.run) {
          void this.api.cancelJob(id, job.jobId ?? '').catch(() => undefined);
          return;
        }
        job = await this.api.getJob(id, job.jobId ?? '');
      }
      if (job.state !== 'done') throw new Error(`Coverage for B ${job.state}.`);
      const gridB = await this.api.getCoverage(id, job.jobId ?? '', link);
      if (run !== this.run) return;
      const d = subtractGrids(a, gridB);
      if (!d) throw new Error('The two coverage grids could not be compared.');
      this.delta.set(d);
      this.diffState.set('done');
    } catch (e) {
      if (run !== this.run) return;
      this.diffState.set('failed');
      this.diffError.set(ApiError.from(e).message);
    }
  }
}
