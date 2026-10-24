import { Component, computed, inject, input } from '@angular/core';
import { FpvWorkspace } from '../core/fpv-workspace.service';

/** Results beyond the radio verdict: link gaps on an autonomous route and the flight check modules. */
@Component({
  selector: 'fpv-flight-checks',
  template: `
    @if (ws.linkGapText(); as gap) {
      <section class="rf-card rf-card--warn card" aria-labelledby="gap-title">
        <b id="gap-title" class="warn">Autonomous: links lost on the way</b>
        <div class="rf-body">{{ gap }}</div>
      </section>
    }
    @if (ws.flightChecks().length) {
      <section class="rf-card card" [class.rf-card--blocked]="blocked()" [class.rf-card--warn]="!blocked()" aria-labelledby="checks-title">
        <b id="checks-title" [class]="blocked() ? 'bad' : 'warn'">Flight checks</b>
        <ul>
          @for (c of ws.flightChecks(); track $index) {
            <li [class]="'tone-' + c.tone">
              @if (c.legIndex !== null && !compact()) {
                <button type="button" class="where" (click)="ws.openWhy(c.legIndex)">{{ c.where }}</button>
              } @else {
                <span class="where">{{ c.where }}</span>
              }
              <span class="mark" [attr.aria-label]="c.tone === 'blocked' ? 'Blocked' : 'Warning'">{{ c.tone === 'blocked' ? '✕' : '!' }}</span>
              <span class="rf-body text">{{ c.text }}</span>
              <span class="module">{{ c.module }}</span>
            </li>
          }
        </ul>
      </section>
    }
  `,
  styles: `
    :host { display: contents; }
    .card { display: flex; flex-direction: column; gap: 8px; padding: 14px; }
    .card > b { font-size: 16px; }
    .warn { color: var(--rf-warn-text); }
    .bad { color: var(--rf-blocked-text); }
    ul { margin: 0; padding: 0; list-style: none; display: flex; flex-direction: column; gap: 10px; }
    li { display: grid; grid-template-columns: 1fr auto; gap: 2px 8px; padding: 8px 10px; border-radius: 8px; background: var(--rf-surface-2); }
    .where { padding: 0; border: 0; background: none; color: var(--rf-text); font: inherit; font-size: 13px; font-weight: 600; text-align: left; }
    button.where { cursor: pointer; text-decoration: underline dotted; }
    .mark { font-weight: 600; text-align: right; }
    .tone-warn .mark { color: var(--rf-warn-text); }
    .tone-blocked .mark { color: var(--rf-blocked-text); }
    .text, .module { grid-column: 1 / -1; }
    .text { font-size: 13px; line-height: 1.45; color: var(--rf-text-3); }
    .module { font-size: 12px; color: var(--rf-text-2); }
  `,
})
export class FlightChecksComponent {
  protected readonly ws = inject(FpvWorkspace);
  /** On the phone the legs open from their own rows. */
  readonly compact = input(false);
  protected readonly blocked = computed(() => this.ws.flightChecks().some((c) => c.tone === 'blocked'));
}
