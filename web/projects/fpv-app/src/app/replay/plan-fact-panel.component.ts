import { Component, computed, inject } from '@angular/core';
import { PlanFactRow, ReplayFormat } from '@rfmap/ui-kit';
import { FlightReplay } from '../core/flight-replay.service';
import { ReplayVm } from './replay-vm';

/**
 * Plan against fact (design 08): the planned route and what the planner predicted next to what
 * the log shows was flown, then what that means for the next plan, leg by leg.
 */
@Component({
  selector: 'fpv-plan-fact-panel',
  template: `
    @if (plan(); as p) {
      <div class="rf-eyebrow">Planned against flown</div>
      @if (!p.evaluated) {
        <div class="rf-card rf-card--info note">The route has not been calculated since it last changed, so link margins and energy have no plan to compare with. Calculate the route in Plan.</div>
      }
      <table>
        <thead><tr><th>Item</th><th>Plan</th><th>Flown</th><th>Diff</th></tr></thead>
        <tbody>
          @for (r of p.rows; track r.metric) {
            <tr>
              <td>{{ r.label }}</td>
              <td class="rf-mono">{{ value(r, 'plan') }}</td>
              <td class="rf-mono">{{ value(r, 'fact') }}</td>
              <td class="rf-mono">{{ delta(r) }}</td>
            </tr>
          }
        </tbody>
      </table>
      @for (f of p.findings; track $index) {
        <div class="rf-card note" [class.rf-card--warn]="f.severity === 'warn'" [class.rf-card--blocked]="f.severity === 'blocked'">{{ f.text }}</div>
      }
      @if (p.legs.length) {
        <div class="rf-eyebrow">Leg by leg</div>
        <table>
          <thead><tr><th>Leg</th><th>Margin</th><th>Link</th><th>Off route</th></tr></thead>
          <tbody>
            @for (l of p.legs; track l.index) {
              <tr [class.bad]="(l.minLinkPct ?? 100) < 30">
                <td>{{ l.from }} → {{ l.to }}</td>
                <td class="rf-mono">{{ margin(l.controlMarginDb) }}</td>
                <td class="rf-mono">{{ l.minLinkPct === null ? '–' : round(l.minLinkPct) + ' %' }}</td>
                <td class="rf-mono">{{ l.flownS > 0 ? distance(l.maxOffRouteM) : 'not flown' }}</td>
              </tr>
            }
          </tbody>
        </table>
      }
    } @else if (replay.view()) {
      <p class="rf-body muted">This flight was opened without keeping it, so there is no plan to compare it with. Load it again with “Save to the logbook” on.</p>
    } @else {
      <p class="rf-body muted">Open a flight of this mission to compare it with the plan.</p>
      <button type="button" class="rf-btn rf-btn--primary" (click)="replay.importOpen.set(true)">Load log</button>
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 12px; }
    .muted { color: var(--rf-text-3); }
    .note { font-size: 12px; }
    table { width: 100%; border-collapse: collapse; font-size: 12px; }
    th { text-align: right; font-weight: 600; color: var(--rf-text-2); padding: 4px 6px; border-bottom: 1px solid var(--rf-border); }
    td { text-align: right; padding: 6px; border-bottom: 1px solid var(--rf-border); }
    th:first-child, td:first-child { text-align: left; }
    td:first-child { color: var(--rf-text-2); }
    tr.bad td { color: var(--rf-warn-text); }
  `,
})
export class PlanFactPanelComponent {
  protected readonly replay = inject(FlightReplay);
  protected readonly plan = computed(() => this.replay.view()?.plan ?? null);

  protected value(r: PlanFactRow, which: 'plan' | 'fact'): string {
    return ReplayVm.planValue(r, which);
  }

  protected delta(r: PlanFactRow): string {
    return ReplayVm.planDelta(r);
  }

  protected margin(db: number | null): string {
    return db === null ? '–' : `${db >= 0 ? '+' : '−'}${Math.abs(db).toFixed(1)} dB`;
  }

  protected distance(m: number): string {
    return ReplayFormat.distance(m);
  }

  protected round(v: number): number {
    return Math.round(v);
  }
}
