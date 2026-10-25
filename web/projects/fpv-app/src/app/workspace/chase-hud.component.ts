import { Component, DestroyRef, computed, effect, inject } from '@angular/core';
import { RfFormat } from '@rfmap/ui-kit';
import { CorridorState } from '../core/fpv-altitude';
import { CHASE_RATES, ChaseRate, ChaseView } from '../core/fpv-chase';

const STATE_TEXT: Record<CorridorState, string> = {
  inside: 'Inside the corridor',
  takeoff: 'Climb-out zone',
  low: 'Under the floor',
  ground: 'Below the ground',
  high: 'Over the ceiling',
};
/** Under this the plan counts as level, m/s. */
const LEVEL_MPS = 0.2;
/** Over the drone's rate by more than this, the climb shows as too steep; the module's own slack. */
const RATE_SLACK_MPS = 0.05;

function clock(seconds: number): string {
  const t = Math.max(0, Math.round(seconds));
  return `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`;
}

/**
 * The chase camera's heads-up display: the drone's heights and the climb the plan asks for, what
 * comes next, and a timeline of the legs with play, pause and speed. It drives the playback clock.
 */
@Component({
  selector: 'fpv-chase-hud',
  template: `
    @if (chase.frame(); as f) {
      <div class="read" role="status" aria-live="off" aria-label="Chase read-out">
        <div class="big">
          <span class="v rf-mono">{{ m(f.aglM) }}</span><span class="u">m AGL</span>
        </div>
        <div class="row rf-mono">
          <span>{{ m(f.mslM) }} m MSL</span>
          <span [class]="'vs ' + vsTone()">{{ vsText() }}</span>
        </div>
        <div [class]="'state tone-' + stateTone()">{{ stateText() }}</div>
      </div>
      <div class="ahead" aria-label="Ahead">
        @for (a of chase.ahead(); track a.kind) {
          <div [class]="'a tone-' + a.tone">
            <span>{{ a.kind === 'waypoint' ? 'Next: ' + a.text : a.text }}</span>
            <span class="rf-mono">in {{ km(a.inM) }}</span>
          </div>
        }
      </div>
      <div class="bar" role="group" aria-label="Chase playback">
        <button
          type="button"
          class="rf-btn rf-btn--sm rf-btn--primary play"
          [attr.aria-label]="chase.playing() ? 'Pause' : 'Play'"
          (click)="chase.toggle()"
        >
          {{ chase.playing() ? '❚❚' : '▶' }}
        </button>
        <div class="line">
          <div class="legs" aria-hidden="true">
            @for (l of legs(); track l.leg) {
              <span [class]="'leg tone-' + l.tone" [style.flex-grow]="l.toS - l.fromS" [title]="l.name"></span>
            }
          </div>
          <input
            type="range"
            min="0"
            [max]="length()"
            step="10"
            [value]="f.s"
            (input)="seek($event)"
            [attr.aria-valuetext]="km(f.s) + ', ' + legName()"
            aria-label="Position along the route"
          />
        </div>
        <span class="time rf-mono">{{ time(f.timeS) }} / {{ time(duration()) }}</span>
        <div class="rates" role="radiogroup" aria-label="Playback speed">
          @for (r of rates; track r) {
            <button
              type="button"
              role="radio"
              class="rate"
              [class.on]="chase.rate() === r"
              [attr.aria-checked]="chase.rate() === r"
              (click)="setRate(r)"
            >
              ×{{ r }}
            </button>
          }
        </div>
        <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="chase.stop()">Exit chase</button>
      </div>
    }
  `,
  styles: `
    :host { position: absolute; inset: 0; z-index: 3; pointer-events: none; }
    .read, .ahead, .bar { pointer-events: auto; background: rgba(16, 22, 29, 0.92); border: 1px solid var(--rf-border); border-radius: 10px; }
    .read { position: absolute; left: 14px; top: 64px; padding: 10px 14px; display: flex; flex-direction: column; gap: 4px; min-width: 170px; }
    .big { display: flex; align-items: baseline; gap: 6px; }
    .big .v { font-size: 34px; font-weight: 600; color: var(--rf-text); line-height: 1; }
    .big .u { color: var(--rf-text-2); font-size: 13px; }
    .row { display: flex; gap: 12px; font-size: 13px; color: var(--rf-text-2); }
    .vs.up { color: var(--rf-ok-text); }
    .vs.down { color: var(--rf-info-text, #8fb8ff); }
    .vs.over { color: var(--rf-warn-text); }
    .state { font-size: 12px; }
    .tone-ok { color: var(--rf-ok-text); }
    .tone-warn { color: var(--rf-warn-text); }
    .tone-bad { color: var(--rf-blocked-text); }
    .ahead { position: absolute; right: 14px; top: 64px; padding: 8px 12px; display: flex; flex-direction: column; gap: 4px; min-width: 200px; font-size: 13px; }
    .ahead:empty { display: none; }
    .a { display: flex; justify-content: space-between; gap: 12px; color: var(--rf-text); }
    .a.tone-warn { color: var(--rf-warn-text); }
    .bar { position: absolute; left: 14px; right: 14px; bottom: calc(var(--fpv-cover-bottom, 0px) + 16px); padding: 8px 10px; display: flex; align-items: center; gap: 10px; font-size: 12px; color: var(--rf-text-2); }
    .play { min-width: 40px; }
    .line { position: relative; flex: 1; min-width: 80px; height: 24px; }
    .legs { position: absolute; left: 0; right: 0; top: 9px; height: 6px; display: flex; gap: 2px; }
    .leg { flex-basis: 0; border-radius: 3px; background: var(--rf-ok); opacity: 0.7; }
    .leg.tone-warn { background: var(--rf-warn); }
    .leg.tone-bad { background: var(--rf-blocked); }
    .line input { position: absolute; inset: 0; width: 100%; margin: 0; background: transparent; accent-color: var(--rf-text); }
    .time { white-space: nowrap; color: var(--rf-text); }
    .rates { display: inline-flex; border: 1px solid var(--rf-border); border-radius: 6px; overflow: hidden; }
    .rate { border: 0; background: transparent; color: var(--rf-text-2); padding: 3px 8px; font: inherit; }
    .rate.on { background: var(--rf-surface-3); color: var(--rf-text); font-weight: 600; }
    :host-context(.phone) .read { top: 110px; left: 12px; padding: 8px 10px; min-width: 0; }
    :host-context(.phone) .big .v { font-size: 26px; }
    :host-context(.phone) .ahead { top: auto; bottom: 72px; left: 12px; right: 12px; min-width: 0; }
    :host-context(.phone) .bar { left: 8px; right: 8px; bottom: 8px; flex-wrap: wrap; }
    :host-context(.phone) .rates, :host-context(.phone) .time { display: none; }
  `,
})
export class ChaseHudComponent {
  protected readonly chase = inject(ChaseView);
  protected readonly rates = CHASE_RATES;

  protected readonly legs = computed(() => this.chase.track()?.legs ?? []);
  protected readonly length = computed(() => this.chase.track()?.length ?? 0);
  protected readonly duration = computed(() => this.chase.track()?.durationS ?? 0);
  protected readonly legName = computed(() => {
    const f = this.chase.frame();
    return this.legs().find((l) => l.leg === f?.leg)?.name ?? '';
  });
  protected readonly stateText = computed(() => {
    const f = this.chase.frame();
    if (!f) return '';
    const text = STATE_TEXT[f.state];
    return f.state === 'low' || f.state === 'ground' ? `${text}: needs ${Math.round(f.needAglM)} m AGL` : text;
  });
  protected readonly stateTone = computed(() => {
    const st = this.chase.frame()?.state;
    if (st === 'ground') return 'bad';
    return st === 'low' || st === 'high' ? 'warn' : 'ok';
  });
  /** The climb the plan asks for here, against the drone's rates. */
  protected readonly vsText = computed(() => {
    const v = this.chase.frame()?.verticalMps ?? 0;
    if (Math.abs(v) < LEVEL_MPS) return 'Level';
    return `${v > 0 ? 'Climb' : 'Descend'} ${RfFormat.format(Math.abs(v), 'mps')}`;
  });
  protected readonly vsTone = computed(() => {
    const v = this.chase.frame()?.verticalMps ?? 0;
    const lim = this.chase.limits();
    if (Math.abs(v) < LEVEL_MPS) return '';
    if (lim && (v > lim.climbMps + RATE_SLACK_MPS || -v > lim.descentMps + RATE_SLACK_MPS)) return 'over';
    return v > 0 ? 'up' : 'down';
  });

  private last: number | null = null;
  private raf = 0;

  constructor() {
    effect((onCleanup) => {
      if (!this.chase.playing()) return;
      this.last = null;
      const step = (now: number) => {
        if (this.last !== null) this.chase.tick(Math.min(0.25, (now - this.last) / 1000));
        this.last = now;
        this.raf = requestAnimationFrame(step);
      };
      this.raf = requestAnimationFrame(step);
      onCleanup(() => cancelAnimationFrame(this.raf));
    });
    inject(DestroyRef).onDestroy(() => cancelAnimationFrame(this.raf));
  }

  protected m(v: number): string {
    return String(Math.round(v));
  }

  protected km(m: number): string {
    return RfFormat.format(m, 'km1');
  }

  protected time(s: number): string {
    return clock(s);
  }

  protected seek(e: Event): void {
    this.chase.seek(Number((e.target as HTMLInputElement).value));
  }

  protected setRate(r: ChaseRate): void {
    this.chase.rate.set(r);
  }
}
