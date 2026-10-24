import { Component, computed, inject } from '@angular/core';
import { MissionStore, RfFormat } from '@rfmap/ui-kit';
import { AltitudeView, FpvClimbStats, FpvClimbStrategy } from '../core/fpv-altitude';

type Trend = 'better' | 'worse' | 'same';

interface DeltaVm {
  label: string;
  text: string;
  trend: Trend;
  title: string;
}

interface CardVm {
  s: FpvClimbStrategy;
  recommended: boolean;
  shown: boolean;
  deltas: DeltaVm[];
  warnings: string[];
  waypoints: string;
}

/** Below these a difference from the plan reads as the same. */
const SAME_WH = 0.05;
const SAME_M = 0.5;
const SAME_S = 0.5;

function trendOf(delta: number, same: number): Trend {
  if (Math.abs(delta) < same) return 'same';
  return delta < 0 ? 'better' : 'worse';
}

function signedWh(v: number): string {
  const text = Math.abs(v) < SAME_WH ? '0.0' : `${v > 0 ? '+' : ''}${v.toFixed(1)}`;
  return `${RfFormat.minus(text)} Wh`;
}

function signedTime(seconds: number): string {
  const whole = Math.round(Math.abs(seconds));
  const text = `${Math.floor(whole / 60)}:${String(whole % 60).padStart(2, '0')}`;
  if (whole === 0) return text;
  return seconds > 0 ? `+${text}` : `${RfFormat.MINUS}${text}`;
}

/**
 * The climb assistant beside the altitude profile: each strategy from fpv.altitude against the
 * plan (energy, or the total climb without a drone, the flight time and the lowest height over the
 * floor), drawn over the profile on request, and applied only through the fix preview.
 */
@Component({
  selector: 'fpv-climb-assistant',
  template: `
    @if (climb(); as cl) {
      <h3 id="climb-title">Climb assistant</h3>
      <p class="lead">
        Against the plan: {{ cl.energyEstimated ? 'route energy' : 'total climb' }}, flight time and
        the lowest height over the safe floor.
      </p>
      <ul class="cards" aria-labelledby="climb-title">
        @for (c of cards(); track c.s.id) {
          <li class="card" [class.is-recommended]="c.recommended" [class.is-shown]="c.shown">
            <div class="top">
              <h4>{{ c.s.title }}</h4>
              @if (c.recommended) {
                <span class="badge">Recommended</span>
              }
            </div>
            <p class="hint">{{ c.s.summary }}</p>
            <dl class="deltas">
              @for (d of c.deltas; track d.label) {
                <div [title]="d.title">
                  <dt>{{ d.label }}</dt>
                  <dd class="rf-mono" [class]="'is-' + d.trend">{{ d.text }}</dd>
                </div>
              }
            </dl>
            <p class="wps">{{ c.waypoints }}</p>
            @for (w of c.warnings; track w) {
              <p class="warn">{{ w }}</p>
            }
            <div class="actions">
              <button
                type="button"
                class="rf-btn rf-btn--sm"
                [attr.aria-pressed]="c.shown"
                (click)="toggle(c.s)"
              >
                {{ c.shown ? 'Hide from profile' : 'Show on profile' }}
              </button>
              @if (store.canEdit()) {
                <button
                  type="button"
                  class="rf-btn rf-btn--sm rf-btn--primary"
                  [disabled]="view.stale() || !!store.preview() || store.busy()"
                  [title]="view.stale() ? 'Recalculate first: the mission changed since' : ''"
                  (click)="preview(c.s)"
                >
                  Preview fix
                </button>
              }
            </div>
          </li>
        }
      </ul>
      @if (cl.notes.length) {
        <details class="notes">
          <summary>How these are worked out</summary>
          @for (n of cl.notes; track $index) {
            <p>{{ n }}</p>
          }
        </details>
      }
    }
  `,
  styles: `
    :host {
      display: flex;
      flex-direction: column;
      gap: 8px;
      font-size: 12px;
      min-width: 0;
    }
    h3 {
      margin: 0;
      font-size: 13px;
    }
    .lead,
    .hint,
    .wps,
    .warn,
    .notes p {
      margin: 0;
      line-height: 1.45;
    }
    .lead,
    .wps,
    .notes {
      color: var(--rf-text-3);
    }
    .cards {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 8px;
      max-height: 320px;
      overflow-y: auto;
    }
    .card {
      display: flex;
      flex-direction: column;
      gap: 6px;
      padding: 8px 10px;
      border: 1px solid var(--rf-border);
      border-radius: 8px;
      background: var(--rf-surface-2);
    }
    .card.is-recommended {
      border-color: #35d0ba;
    }
    .card.is-shown {
      box-shadow: inset 3px 0 0 #c08cff;
    }
    .top {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    h4 {
      margin: 0;
      font-size: 12px;
    }
    .badge {
      margin-left: auto;
      font-size: 10px;
      padding: 1px 6px;
      border-radius: 999px;
      background: rgba(53, 208, 186, 0.18);
      color: var(--rf-ok-text);
    }
    .hint {
      color: var(--rf-text-2);
    }
    .deltas {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 6px;
      margin: 0;
    }
    .deltas dt {
      color: var(--rf-text-3);
      font-size: 10px;
    }
    .deltas dd {
      margin: 0;
      font-weight: 600;
    }
    dd.is-better {
      color: var(--rf-ok-text);
    }
    dd.is-worse {
      color: var(--rf-warn-text);
    }
    .warn {
      color: var(--rf-warn-text);
    }
    .actions {
      display: flex;
      gap: 6px;
      flex-wrap: wrap;
    }
  `,
})
export class ClimbAssistantComponent {
  protected readonly view = inject(AltitudeView);
  protected readonly store = inject(MissionStore);

  protected readonly climb = this.view.climb;

  protected readonly cards = computed<CardVm[]>(() => {
    const cl = this.climb();
    if (!cl) return [];
    const shown = this.view.ghostStrategy()?.id ?? null;
    return cl.strategies.map((s) => ({
      s,
      recommended: s.id === cl.recommended,
      shown: s.id === shown,
      deltas: this.deltas(s, cl.plan, cl.energyEstimated),
      warnings: this.warnings(s, cl.climbRateMps),
      waypoints: this.waypointsText(s),
    }));
  });

  protected toggle(s: FpvClimbStrategy): void {
    this.view.ghost.set(this.view.ghostStrategy()?.id === s.id ? null : s.id);
  }

  protected preview(s: FpvClimbStrategy): void {
    this.view.ghost.set(s.id);
    this.store.previewFix(s.fix);
  }

  private deltas(s: FpvClimbStrategy, plan: FpvClimbStats, energy: boolean): DeltaVm[] {
    const out: DeltaVm[] = [];
    if (energy && s.energyWh !== undefined && plan.energyWh !== undefined) {
      const d = s.energyWh - plan.energyWh;
      out.push({
        label: 'Energy',
        text: signedWh(d),
        trend: trendOf(d, SAME_WH),
        title: `${s.energyWh.toFixed(1)} Wh against ${plan.energyWh.toFixed(1)} Wh planned`,
      });
    } else {
      const d = s.riseM - plan.riseM;
      out.push({
        label: 'Climb',
        text: RfFormat.format(d, 'm+'),
        trend: trendOf(d, SAME_M),
        title: `${RfFormat.format(s.riseM, 'm')} in all against ${RfFormat.format(plan.riseM, 'm')} planned`,
      });
    }
    const dt = s.timeS - plan.timeS;
    out.push({
      label: 'Time',
      text: signedTime(dt),
      trend: trendOf(dt, SAME_S),
      title: 'Flight time against the plan',
    });
    const dc = s.minClearanceM - plan.minClearanceM;
    out.push({
      label: 'Over floor',
      text: RfFormat.format(s.minClearanceM, 'm+'),
      // More room over the floor is better; under it is worse whatever the plan does.
      trend: s.minClearanceM < -0.05 ? 'worse' : trendOf(-dc, SAME_M),
      title: `Lowest height over the safe floor past the take-off zone; the plan: ${RfFormat.format(plan.minClearanceM, 'm+')}`,
    });
    return out;
  }

  private waypointsText(s: FpvClimbStrategy): string {
    const added = s.addedWaypoints;
    if (added === 0) return `${s.waypoints} waypoints, none added`;
    return `${s.waypoints} waypoints, ${added} added to follow the profile`;
  }

  private warnings(s: FpvClimbStrategy, climbRateMps: number): string[] {
    const out: string[] = [];
    if (!s.withinClimbRate)
      out.push(
        `Climbs at up to ${RfFormat.format(s.steepestClimbMps, 'mps')}: faster than the drone's ${RfFormat.format(climbRateMps, 'mps')}.`,
      );
    if (s.overCeilingM > 0.05)
      out.push(`Up to ${RfFormat.format(s.overCeilingM, 'm')} over the ceiling.`);
    return out;
  }
}
