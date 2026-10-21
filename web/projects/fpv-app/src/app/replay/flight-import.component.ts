import { Component, DestroyRef, ElementRef, afterNextRender, computed, inject, signal, viewChild } from '@angular/core';
import { ApiError, FlightLogPreview, FlightView, MissionStore, ReplayFormat, RfmapApiClient } from '@rfmap/ui-kit';
import { FlightReplay } from '../core/flight-replay.service';
import { ReplayVm } from './replay-vm';

interface PickedFile {
  file: File;
  state: 'reading' | 'done' | 'error';
  preview?: FlightLogPreview;
  error?: string;
  seconds?: number;
}

interface Choice {
  file: number;
  flight: number;
}

const MAX_BYTES = 64 * 1024 * 1024;

/**
 * Open a flight log (design 12): drop or choose files, see every flight in them and why a file has
 * none, pick the flight to replay, and keep the flights in the mission's logbook or only look.
 */
@Component({
  selector: 'fpv-flight-import',
  template: `
    <div class="backdrop" (click)="close()"></div>
    <section #dialog class="dialog" role="dialog" aria-modal="true" aria-labelledby="import-title" tabindex="-1" (keydown.escape)="close()">
      <div class="head">
        <h2 id="import-title">Open a flight log</h2>
        <button type="button" class="rf-icon-btn" aria-label="Close" (click)="close()">✕</button>
      </div>
      <label class="drop" [class.over]="over()" (dragover)="dragOver($event)" (dragleave)="over.set(false)" (drop)="dropped($event)">
        <input type="file" multiple accept=".bbl,.bfl,.txt,.BBL,.BFL,.TXT" (change)="chosen($event)" />
        <b>Drop Blackbox files here or choose them</b>
        <span>.bbl, .bfl, .txt from Betaflight · up to 64 MB each</span>
      </label>
      @for (f of files(); track $index; let fi = $index) {
        <div class="file" [class.empty]="f.state === 'done' && !replayable(f).length">
          <div class="file-head">
            <b>{{ f.file.name }}</b>
            <span class="rf-caption">{{ size(f.file.size) }}{{ summaryOf(f) }}</span>
          </div>
          @if (f.state === 'reading') {
            <span class="rf-caption" role="status">Reading…</span>
          } @else if (f.state === 'error') {
            <div class="rf-card rf-card--blocked note" role="alert">{{ f.error }}</div>
          } @else {
            @for (v of f.preview!.flights; track v.index; let vi = $index) {
              @if (v.status === 'header-only' || v.status === 'unsupported') {
                <div class="rf-card rf-card--warn note">
                  <b>Flight {{ v.index }} has no flight data</b>
                  @for (n of v.notes; track n.code) {<span>{{ n.message }}{{ n.fix ? ' ' + n.fix : '' }}</span>}
                </div>
              } @else {
                <label class="opt">
                  <input type="radio" name="flight" [checked]="isChosen(fi, vi)" (change)="choice.set({ file: fi, flight: vi })" />
                  <span class="opt-main">
                    <b>Flight {{ v.index }} · {{ time(v) }}</b>
                    <span class="rf-mono">{{ line(v) }} · {{ v.channels['gps'] && v.summary.distanceM !== undefined ? 'GPS' : 'no GPS track' }}</span>
                  </span>
                  @for (w of v.warnings; track $index) {<span class="chip">{{ w }}</span>}
                </label>
              }
            }
          }
        </div>
      }
      @if (canSave()) {
        <label class="save"><input type="checkbox" [checked]="save()" (change)="save.set(!save())" /> Save to the logbook. Otherwise the log is shown and not kept.</label>
      } @else {
        <span class="rf-caption">The log is shown and not kept: {{ store.missionId() ? 'this link can only view the mission' : 'calculate the mission once to keep flights in it' }}.</span>
      }
      @if (error(); as e) {
        <div class="rf-card rf-card--blocked note" role="alert">{{ e }}</div>
      }
      <div class="actions">
        <button type="button" class="rf-btn rf-btn--secondary" (click)="close()">Cancel</button>
        <button type="button" class="rf-btn rf-btn--primary" (click)="replayChosen()" [disabled]="!chosenView() || busy()">
          {{ busy() ? 'Saving…' : 'Replay flight ' + (chosenView()?.index ?? 1) }}
        </button>
      </div>
    </section>
  `,
  styles: `
    :host { position: absolute; inset: 0; z-index: 20; display: flex; align-items: center; justify-content: center; }
    .backdrop { position: absolute; inset: 0; background: rgba(11, 15, 20, 0.6); }
    .dialog { position: relative; width: 520px; max-width: calc(100% - 32px); max-height: calc(100% - 32px); overflow-y: auto; padding: 20px; border-radius: 16px; background: var(--rf-surface-2); border: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 12px; }
    .head { display: flex; justify-content: space-between; align-items: center; }
    h2 { margin: 0; font-size: 18px; }
    .drop { display: flex; flex-direction: column; align-items: center; gap: 4px; padding: 24px; border: 2px dashed var(--rf-border-strong); border-radius: 12px; cursor: pointer; text-align: center; }
    .drop.over { border-color: var(--rf-ok); background: var(--rf-ok-bg); }
    .drop input { position: absolute; width: 1px; height: 1px; opacity: 0; }
    .drop span { font-size: 12px; color: var(--rf-text-2); }
    .file { display: flex; flex-direction: column; gap: 6px; padding: 10px; border-radius: 10px; border: 1px solid var(--rf-border); }
    .file-head { display: flex; justify-content: space-between; gap: 8px; align-items: baseline; }
    .note { display: flex; flex-direction: column; gap: 4px; font-size: 12px; }
    .opt { display: flex; align-items: center; gap: 10px; padding: 6px; border-radius: 8px; cursor: pointer; flex-wrap: wrap; }
    .opt:hover { background: var(--rf-surface-1); }
    .opt input { width: 18px; height: 18px; accent-color: var(--rf-ok); margin: 0; }
    .opt-main { display: flex; flex-direction: column; gap: 2px; flex: 1; font-size: 13px; }
    .opt-main span { font-size: 12px; color: var(--rf-text-2); }
    .chip { padding: 1px 6px; border-radius: 999px; background: var(--rf-warn-bg); color: var(--rf-warn-text); border: 1px solid var(--rf-warn-border); font-size: 11px; }
    .save { display: flex; gap: 8px; align-items: center; font-size: 13px; }
    .actions { display: flex; justify-content: flex-end; gap: 8px; }
  `,
})
export class FlightImportComponent {
  protected readonly replay = inject(FlightReplay);
  protected readonly store = inject(MissionStore);
  private readonly api = inject(RfmapApiClient);
  private readonly dialog = viewChild<ElementRef<HTMLElement>>('dialog');
  private destroyed = false;

  protected readonly files = signal<PickedFile[]>([]);
  protected readonly choice = signal<Choice | null>(null);
  protected readonly over = signal(false);
  protected readonly busy = signal(false);
  protected readonly error = signal<string | null>(null);
  protected readonly canSave = computed(() => this.replay.canEdit());
  protected readonly save = signal(true);
  protected readonly chosenView = computed<FlightView | null>(() => {
    const c = this.choice();
    return c ? (this.files()[c.file]?.preview?.flights[c.flight] ?? null) : null;
  });

  constructor() {
    afterNextRender(() => this.dialog()?.nativeElement.focus());
    inject(DestroyRef).onDestroy(() => (this.destroyed = true));
  }

  protected close(): void {
    this.replay.importOpen.set(false);
  }

  protected dragOver(e: DragEvent): void {
    e.preventDefault();
    this.over.set(true);
  }

  protected dropped(e: DragEvent): void {
    e.preventDefault();
    this.over.set(false);
    this.add(Array.from(e.dataTransfer?.files ?? []));
  }

  protected chosen(e: Event): void {
    const input = e.target as HTMLInputElement;
    this.add(Array.from(input.files ?? []));
    input.value = '';
  }

  /** Decodes each file on the server to show its flights; nothing is kept yet. */
  add(files: File[]): void {
    for (const file of files) {
      const index = this.files().length;
      this.files.update((list) => [...list, { file, state: 'reading' }]);
      if (file.size > MAX_BYTES) {
        this.patch(index, { state: 'error', error: `${file.name} is ${this.size(file.size)}; files up to 64 MB are read.` });
        continue;
      }
      void this.read(index, file);
    }
  }

  private async read(index: number, file: File): Promise<void> {
    const started = performance.now();
    try {
      const preview = await this.api.decodeFlightLog(this.store.missionId(), file, file.name);
      this.patch(index, { state: 'done', preview, seconds: (performance.now() - started) / 1000 });
      if (!this.choice()) {
        const first = preview.flights.findIndex((v) => FlightImportComponent.isReplayable(v));
        if (first >= 0) this.choice.set({ file: index, flight: first });
      }
    } catch (e) {
      this.patch(index, { state: 'error', error: ApiError.from(e).message });
    }
  }

  private patch(index: number, p: Partial<PickedFile>): void {
    this.files.update((list) => list.map((f, i) => (i === index ? { ...f, ...p } : f)));
  }

  static isReplayable(v: FlightView): boolean {
    return v.status === 'ok' || v.status === 'partial';
  }

  protected replayable(f: PickedFile): FlightView[] {
    return (f.preview?.flights ?? []).filter((v) => FlightImportComponent.isReplayable(v));
  }

  protected isChosen(file: number, flight: number): boolean {
    const c = this.choice();
    return !!c && c.file === file && c.flight === flight;
  }

  protected summaryOf(f: PickedFile): string {
    if (f.state !== 'done' || !f.preview) return '';
    const n = this.replayable(f).length;
    const read = f.seconds === undefined ? '' : ` · read in ${f.seconds.toFixed(1)} s`;
    return ` · ${n} ${n === 1 ? 'flight' : 'flights'}${read}`;
  }

  protected size(bytes: number): string {
    if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
    return `${Math.max(1, Math.round(bytes / 1024))} KB`;
  }

  protected time(v: FlightView): string {
    return ReplayFormat.started(v.startedAt).split(', ').pop() ?? '';
  }

  protected line(v: FlightView): string {
    return ReplayVm.headline(v);
  }

  /** Keeps every file with flights when asked, then puts the chosen flight on screen. */
  protected async replayChosen(): Promise<void> {
    const c = this.choice();
    const view = this.chosenView();
    if (!c || !view) return;
    if (!this.canSave() || !this.save()) {
      this.replay.show(view);
      this.close();
      return;
    }
    this.busy.set(true);
    this.error.set(null);
    const mission = this.store.missionId();
    // The dialog may close, or another mission open, while the files are saved.
    const gone = () => this.destroyed || this.store.missionId() !== mission;
    try {
      const keep = await this.keepAll(c.file);
      if (gone()) return;
      await this.replay.refresh();
      if (gone()) return;
      const row = keep?.flights.find((r) => r.index === view.index);
      if (row?.id) await this.replay.open(row.id);
      else this.replay.show(view);
      this.close();
    } catch (e) {
      if (!gone()) this.error.set(ApiError.from(e).message);
    } finally {
      this.busy.set(false);
    }
  }

  /** Imports every readable file; answers the import of `chosenFile`. */
  private async keepAll(chosenFile: number) {
    const missionId = this.store.missionId();
    if (!missionId) return null;
    const kept = this.files()
      .map((f, i) => ({ f, i }))
      .filter(({ f }) => f.state === 'done' && this.replayable(f).length > 0);
    const results = await Promise.all(kept.map(({ f }) => this.api.importFlightLog(missionId, f.file, f.file.name)));
    return results[kept.findIndex(({ i }) => i === chosenFile)] ?? null;
  }
}
