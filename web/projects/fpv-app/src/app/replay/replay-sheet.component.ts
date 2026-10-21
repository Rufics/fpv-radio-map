import { Component, computed, inject } from '@angular/core';
import { ReplayFormat } from '@rfmap/ui-kit';
import { FlightReplay } from '../core/flight-replay.service';
import { ReplayVm } from './replay-vm';

/** Replay on a phone (design 13): the read-out at the replay time, the clock and the events that matter. */
@Component({
  selector: 'fpv-replay-sheet',
  template: `
    <div class="top">
      <button type="button" class="rf-btn rf-btn--sm rf-btn--muted" (click)="replay.setMode('plan')">‹ Plan</button>
      <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="replay.importOpen.set(true)">Load log</button>
    </div>
    @if (replay.view(); as v) {
      <div class="head">
        <b>{{ title() }}</b>
        <span class="rf-caption">{{ started() }} · {{ clock(v.durationS) }}</span>
      </div>
      <div class="clock">
        <button type="button" class="rf-btn rf-btn--sm rf-btn--primary" (click)="replay.toggle()" [attr.aria-label]="replay.playing() ? 'Pause' : 'Play'">{{ replay.playing() ? '❚❚' : '▶' }}</button>
        <input type="range" [min]="start()" [max]="end()" step="0.1" [value]="replay.t()" (input)="seek($event)" aria-label="Replay time" />
        <span class="rf-mono">{{ clock(replay.t()) }}</span>
      </div>
      <div class="tiles">
        @for (k of readout(); track k.label) {
          <div class="tile" [class.warn]="k.tone === 'warn'"><b class="rf-mono">{{ k.value }}</b><span>{{ k.label }}</span></div>
        }
      </div>
      <ul class="events">
        @for (e of important(); track e.i) {
          <li><button type="button" (click)="replay.selectEvent(e.i)" [class]="'ev is-' + e.severity"><span class="rf-mono">{{ clock(e.t) }}</span> {{ e.title }}</button></li>
        }
      </ul>
    } @else {
      <p class="rf-body">Open a Blackbox log to replay the flight on the map.</p>
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 10px; }
    .top { display: flex; justify-content: space-between; }
    .head { display: flex; flex-direction: column; gap: 2px; }
    .clock { display: flex; align-items: center; gap: 10px; }
    .clock input { flex: 1; accent-color: var(--rf-ok); }
    .tiles { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; }
    .tile { display: flex; flex-direction: column; font-size: 11px; color: var(--rf-text-2); }
    .tile b { font-size: 15px; color: var(--rf-text); }
    .tile.warn b { color: var(--rf-warn-text); }
    .events { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 4px; }
    .ev { width: 100%; text-align: left; min-height: 40px; padding: 0 12px; border: 0; border-radius: 10px; background: var(--rf-surface-3); color: var(--rf-text); font: inherit; font-size: 14px; }
    .ev.is-warn { border-left: 4px solid var(--rf-warn); }
    .ev.is-blocked { border-left: 4px solid var(--rf-blocked); }
  `,
})
export class ReplaySheetComponent {
  protected readonly replay = inject(FlightReplay);
  protected readonly start = computed(() => this.replay.timeline()?.start ?? 0);
  protected readonly end = computed(() => this.replay.timeline()?.end ?? 0);
  protected readonly title = computed(() => {
    const v = this.replay.view();
    return v ? ReplayVm.title(v) : '';
  });
  protected readonly started = computed(() => ReplayFormat.started(this.replay.view()?.startedAt));
  /** Four tiles fit a phone: distance, altitude, speed, battery. */
  protected readonly readout = computed(() => {
    const v = this.replay.view();
    const r = this.replay.reading();
    if (!v || !r) return [];
    const wanted = new Set(['From home', 'Altitude', 'Ground speed', 'Battery']);
    return ReplayVm.readout(v, r).filter((k) => wanted.has(k.label));
  });
  protected readonly important = computed(() =>
    (this.replay.view()?.story ?? [])
      .map((e, i) => ({ ...e, i }))
      .filter((e) => e.severity !== 'info')
      .slice(0, 4),
  );

  protected clock(t: number): string {
    return ReplayFormat.clock(t);
  }

  protected seek(e: Event): void {
    this.replay.seek(Number((e.target as HTMLInputElement).value));
  }
}
