import { Injectable, computed, inject, signal } from '@angular/core';
import { Subscription } from 'rxjs';
import { CoverageGrid, Fix, GetMissionResponse, Job, Role, RouteResult } from '../models/api.models';
import { EvaluateMode, LinkKind, Mission, MissionPoint, linksEvaluated } from '../models/mission.models';
import { ApiError, RfmapApiClient } from './api-client';
import { CredentialStore } from './credential-store';
import { applyMergePatch } from './merge-patch';

export type CalcState = 'idle' | 'saving' | 'running' | 'done' | 'cancelled' | 'error';

export interface FixPreview {
  fix: Fix;
  before: Mission;
  after: Mission;
}

/** Which coverage link a job part delivers. @gap parts carry only a name; see API-NOTES.md */
export function coverageLinkOfPart(name: string | undefined): LinkKind | null {
  const n = (name ?? '').toLowerCase();
  if (!n.includes('coverage')) return null;
  if (n.includes('control')) return 'control';
  if (n.includes('video')) return 'video';
  return null;
}

/**
 * Mission state and calculation flow: save (create / optimistic PUT), route calculation,
 * coverage job with per-part progress (finished parts usable at once), cancel,
 * and fixes that are only ever applied through a preview.
 */
@Injectable({ providedIn: 'root' })
export class MissionStore {
  private readonly api = inject(RfmapApiClient);
  private readonly creds = inject(CredentialStore);

  readonly mission = signal<Mission | null>(null);
  readonly missionId = signal<string | null>(null);
  readonly revision = signal(0);
  readonly role = signal<Role>('edit');
  readonly dirty = signal(false);

  readonly result = signal<RouteResult | null>(null);
  /** The result belongs to an older version of the mission. */
  readonly resultStale = signal(false);
  readonly job = signal<Job | null>(null);
  readonly coverage = signal<Partial<Record<LinkKind, CoverageGrid>>>({});
  readonly calcState = signal<CalcState>('idle');
  readonly error = signal<string | null>(null);
  readonly preview = signal<FixPreview | null>(null);

  readonly canEdit = computed(() => this.role() === 'edit');
  readonly evaluated = computed(() => linksEvaluated(this.mission()?.evaluate ?? 'both'));
  /** What the map and lists show: the preview mission while previewing. */
  readonly shownMission = computed(() => this.preview()?.after ?? this.mission());

  private routeSub?: Subscription;
  private pollTimer?: ReturnType<typeof setTimeout>;
  private runId = 0;
  pollIntervalMs = 400;

  // ---------- loading ----------
  newMission(mission: Mission): void {
    this.reset();
    this.mission.set(mission);
    this.missionId.set(null);
    this.role.set('edit');
    this.dirty.set(true);
  }

  loadFromServer(missionId: string, res: GetMissionResponse): void {
    this.reset();
    this.missionId.set(missionId);
    this.mission.set(res.mission);
    this.role.set(res.role);
    this.revision.set(res.revision);
    this.result.set(res.lastResult ?? null);
    this.calcState.set(res.lastResult ? 'done' : 'idle');
    this.dirty.set(false);
  }

  /** Back to the empty state (no mission). */
  clear(): void {
    this.reset();
    this.mission.set(null);
    this.missionId.set(null);
    this.revision.set(0);
    this.role.set('edit');
    this.dirty.set(false);
  }

  async load(missionId: string): Promise<void> {
    const res = await this.api.getMission(missionId);
    this.loadFromServer(missionId, res);
  }

  // ---------- editing ----------
  update(mutator: (m: Mission) => Mission): void {
    const m = this.mission();
    if (!m || !this.canEdit()) return;
    this.mission.set(mutator(structuredClone(m)));
    this.dirty.set(true);
    if (this.result()) this.resultStale.set(true);
  }

  setEvaluate(mode: EvaluateMode): void {
    this.update((m) => ({ ...m, evaluate: mode }));
  }

  setPilot(p: Pick<MissionPoint, 'lat' | 'lon'>): void {
    this.update((m) => ({ ...m, pilot: { ...m.pilot, lat: p.lat, lon: p.lon } }));
  }

  movePoint(index: number, p: Pick<MissionPoint, 'lat' | 'lon'>): void {
    if (index < 0) return this.setPilot(p);
    this.update((m) => ({ ...m, waypoints: m.waypoints.map((w, i) => (i === index ? { ...w, lat: p.lat, lon: p.lon } : w)) }));
  }

  addWaypoint(p: MissionPoint): void {
    this.update((m) => ({ ...m, waypoints: [...m.waypoints, p] }));
  }

  removeWaypoint(index: number): void {
    this.update((m) => (m.waypoints.length <= 1 ? m : { ...m, waypoints: m.waypoints.filter((_, i) => i !== index) }));
  }

  // ---------- fixes: always via preview ----------
  previewFix(fix: Fix): void {
    const before = this.mission();
    if (!before || !fix.patch) return;
    this.preview.set({ fix, before, after: applyMergePatch(before, fix.patch) });
  }

  cancelPreview(): void {
    this.preview.set(null);
  }

  /** Apply the previewed fix and recalculate. */
  async applyPreview(): Promise<void> {
    const p = this.preview();
    if (!p || !this.canEdit()) return;
    this.mission.set(p.after);
    this.preview.set(null);
    this.dirty.set(true);
    await this.calculate();
  }

  // ---------- calculation ----------
  async calculate(): Promise<void> {
    const mission = this.mission();
    if (!mission) return;
    this.cancelRunning(false);
    const run = ++this.runId;
    this.error.set(null);
    this.result.set(null);
    this.resultStale.set(false);
    this.coverage.set({});
    this.job.set(null);
    this.calcState.set('saving');
    try {
      await this.ensureSaved();
    } catch (e) {
      if (run !== this.runId) return;
      this.fail(e);
      return;
    }
    if (run !== this.runId) return;
    this.calcState.set('running');
    const id = this.missionId();

    let routeDone = false;
    let jobDone = false;
    const finish = () => {
      if (run === this.runId && routeDone && jobDone && this.calcState() === 'running') this.calcState.set('done');
    };

    this.routeSub = this.api.calcRoute(id, mission).subscribe({
      next: (r) => {
        if (run !== this.runId) return;
        this.result.set(r);
        routeDone = true;
        finish();
      },
      error: (e) => {
        if (run !== this.runId) return;
        this.fail(e);
      },
    });

    try {
      const job = await this.api.startCoverage(id, mission);
      if (run !== this.runId) return;
      this.job.set(job);
      await this.poll(run, job, () => {
        jobDone = true;
        finish();
      });
    } catch (e) {
      if (run !== this.runId) return;
      // Coverage failing does not hide the route verdict.
      this.error.set(`Coverage: ${ApiError.from(e).message}`);
      jobDone = true;
      finish();
    }
  }

  private async poll(run: number, job: Job, onDone: () => void): Promise<void> {
    const fetched = new Set<LinkKind>();
    const step = async (j: Job) => {
      if (run !== this.runId) return;
      this.job.set(j);
      for (const part of j.parts ?? []) {
        const link = coverageLinkOfPart(part.name);
        if (link && (part.state === 'done' || part.state === 'cached') && !fetched.has(link) && j.jobId) {
          fetched.add(link);
          this.api
            .getCoverage(this.missionId(), j.jobId, link)
            .then((grid) => {
              if (run === this.runId) this.coverage.update((c) => ({ ...c, [link]: grid }));
            })
            .catch(() => fetched.delete(link));
        }
      }
      if (j.state === 'done' || j.state === 'failed' || j.state === 'cancelled') {
        if (j.state === 'failed') this.error.set('Coverage job failed; route results are still valid.');
        onDone();
        return;
      }
      this.pollTimer = setTimeout(async () => {
        try {
          if (!j.jobId) return;
          await step(await this.api.getJob(this.missionId(), j.jobId));
        } catch (e) {
          if (run === this.runId) {
            this.error.set(`Coverage: ${ApiError.from(e).message}`);
            onDone();
          }
        }
      }, this.pollIntervalMs);
    };
    await step(job);
  }

  /** Cancel the running calculation (route request and coverage job). */
  async cancel(): Promise<void> {
    const jobId = this.job()?.jobId;
    this.cancelRunning(true);
    this.calcState.set('cancelled');
    if (jobId) {
      try {
        await this.api.cancelJob(this.missionId(), jobId);
      } catch {
        /* job may have finished meanwhile */
      }
    }
  }

  private cancelRunning(bumpRun: boolean): void {
    if (bumpRun) this.runId++;
    this.routeSub?.unsubscribe();
    this.routeSub = undefined;
    if (this.pollTimer) clearTimeout(this.pollTimer);
    this.pollTimer = undefined;
  }

  private async ensureSaved(): Promise<void> {
    const m = this.mission();
    if (!m) return;
    const id = this.missionId();
    if (!id) {
      const res = await this.api.createMission(m);
      this.missionId.set(res.missionId);
      this.creds.setOwnerKey(res.missionId, res.ownerKey);
      this.creds.setSession(res.missionId, res.session);
      this.creds.setLastMissionId(res.missionId);
      this.revision.set(1);
      this.role.set('edit');
    } else if (this.dirty() && this.canEdit()) {
      this.revision.set(await this.api.saveMission(id, m, this.revision()));
    }
    this.dirty.set(false);
  }

  private fail(e: unknown): void {
    const err = ApiError.from(e);
    this.error.set(err.status === 409 ? 'Someone saved this mission in between. Reload to see their changes.' : err.message);
    this.calcState.set('error');
    this.cancelRunning(true);
  }

  private reset(): void {
    this.cancelRunning(true);
    this.result.set(null);
    this.resultStale.set(false);
    this.job.set(null);
    this.coverage.set({});
    this.preview.set(null);
    this.error.set(null);
    this.calcState.set('idle');
  }
}
