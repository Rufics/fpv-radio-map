import { Injectable, computed, effect, inject, signal, untracked } from '@angular/core';
import {
  ApiError,
  FlightLogRow,
  FlightStoryEvent,
  FlightView,
  MissionStore,
  ReplayReading,
  ReplayTimeline,
  RfmapApiClient,
  TrackMetric,
  TrackScale,
} from '@rfmap/ui-kit';

/** What the workspace shows: planning, a flight replay, or the replay against the plan. */
export type WorkspaceMode = 'plan' | 'replay' | 'plan-fact';

/** How the replay panel tells the flight: the story first, the numbers, or every flight. */
export type ReplayDetail = 'story' | 'details' | 'logbook';

export const REPLAY_SPEEDS = [1, 2, 4, 8] as const;

/**
 * Flight log replay state: the mission's logbook, the flight on screen, the replay clock and
 * what the track is coloured by. Flights come from the server (stored in the mission) or from a
 * preview of a file that is not kept.
 */
@Injectable({ providedIn: 'root' })
export class FlightReplay {
  private readonly api = inject(RfmapApiClient);
  private readonly store = inject(MissionStore);

  readonly mode = signal<WorkspaceMode>('plan');
  readonly detail = signal<ReplayDetail>('story');
  readonly logbook = signal<FlightLogRow[]>([]);
  readonly view = signal<FlightView | null>(null);
  /** The flight on screen was kept in the mission (it has plan against fact) */
  readonly stored = computed(() => !!this.view()?.id);
  readonly t = signal(0);
  readonly playing = signal(false);
  readonly speed = signal<(typeof REPLAY_SPEEDS)[number]>(1);
  readonly colourBy = signal<TrackMetric>('battery');
  readonly selected = signal<number | null>(null);
  readonly importOpen = signal(false);
  readonly canEdit = computed(() => this.store.canEdit() && !!this.store.missionId());
  readonly busy = signal(false);
  readonly error = signal<string | null>(null);

  readonly timeline = computed(() => {
    const v = this.view();
    return v ? new ReplayTimeline(v) : null;
  });
  readonly reading = computed<ReplayReading | null>(() => this.timeline()?.reading(this.t()) ?? null);
  /** Colour metrics this flight has data for */
  readonly metrics = computed(() => {
    const v = this.view();
    return v ? TrackScale.METRICS.filter((m) => TrackScale.available(m, v.timeline)) : [];
  });
  readonly attention = computed(() => (this.view()?.story ?? []).filter((e) => e.severity !== 'info').length);

  private frame: number | null = null;
  private lastFrameMs = 0;

  constructor() {
    // Another mission opened: the flights on screen belong to the one before.
    let shown = this.store.missionId();
    effect(() => {
      const id = this.store.missionId();
      if (id === shown) return;
      shown = id;
      untracked(() => this.clear());
    });
  }

  private clear(): void {
    this.pause();
    this.view.set(null);
    this.logbook.set([]);
    this.selected.set(null);
    this.error.set(null);
  }

  /** Switches the workspace; replay opens the import dialog when the mission has no flights yet. */
  async setMode(mode: WorkspaceMode): Promise<void> {
    this.mode.set(mode);
    if (mode === 'plan') {
      this.pause();
      return;
    }
    await this.refresh();
    // The user may have switched again while the logbook loaded.
    if (this.mode() !== mode) return;
    if (!this.view() && this.logbook().length > 0) await this.open(this.logbook()[0].id ?? '');
    if (this.mode() !== mode) return;
    if (!this.view() && this.logbook().length === 0) this.importOpen.set(true);
  }

  async refresh(): Promise<void> {
    const id = this.store.missionId();
    if (!id) return;
    try {
      this.logbook.set(await this.api.listFlightLogs(id));
    } catch (e) {
      this.error.set(ApiError.from(e).message);
    }
  }

  async open(logId: string): Promise<void> {
    const id = this.store.missionId();
    if (!id || !logId) return;
    this.busy.set(true);
    try {
      this.show(await this.api.getFlightLog(id, logId));
    } catch (e) {
      this.error.set(ApiError.from(e).message);
    } finally {
      this.busy.set(false);
    }
  }

  /** Puts a flight on screen from the start. */
  show(view: FlightView): void {
    this.pause();
    this.error.set(null);
    this.view.set(view);
    this.selected.set(null);
    this.t.set(view.timeline.t[0] ?? 0);
    if (!TrackScale.available(this.colourBy(), view.timeline)) this.colourBy.set(this.metrics()[0] ?? 'battery');
    if (this.mode() === 'plan') this.mode.set('replay');
  }

  async remove(logId: string): Promise<void> {
    const id = this.store.missionId();
    if (!id) return;
    try {
      await this.api.deleteFlightLog(id, logId);
      if (this.view()?.id === logId) this.view.set(null);
      await this.refresh();
    } catch (e) {
      this.error.set(ApiError.from(e).message);
    }
  }

  seek(t: number): void {
    const tl = this.timeline();
    if (!tl) return;
    this.t.set(Math.min(tl.end, Math.max(tl.start, t)));
  }

  selectEvent(index: number): void {
    const e = this.view()?.story[index];
    if (!e) return;
    this.selected.set(index);
    this.seek(e.t);
  }

  step(by: 1 | -1): void {
    const story = this.view()?.story ?? [];
    if (!story.length) return;
    const now = this.selected() ?? (by > 0 ? -1 : story.length);
    this.selectEvent(Math.min(story.length - 1, Math.max(0, now + by)));
  }

  selectedEvent(): FlightStoryEvent | null {
    const i = this.selected();
    return i === null ? null : (this.view()?.story[i] ?? null);
  }

  toggle(): void {
    if (this.playing()) this.pause();
    else this.play();
  }

  play(): void {
    const tl = this.timeline();
    if (!tl || this.playing()) return;
    if (this.t() >= tl.end) this.t.set(tl.start);
    this.playing.set(true);
    this.lastFrameMs = performance.now();
    this.frame = requestAnimationFrame((ms) => this.tick(ms));
  }

  pause(): void {
    this.playing.set(false);
    if (this.frame !== null) cancelAnimationFrame(this.frame);
    this.frame = null;
  }

  private tick(ms: number): void {
    const tl = this.timeline();
    if (!tl || !this.playing()) return;
    const next = this.t() + ((ms - this.lastFrameMs) / 1000) * this.speed();
    this.lastFrameMs = ms;
    if (next >= tl.end) {
      this.t.set(tl.end);
      this.pause();
      return;
    }
    this.t.set(next);
    this.frame = requestAnimationFrame((m) => this.tick(m));
  }
}
