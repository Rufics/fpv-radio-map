import { Component, computed, input } from '@angular/core';
import { BudgetTerm } from '../models/api.models';
import { RfFormat } from '../format/rf-format';

export interface WaterfallStep {
  term: BudgetTerm;
  from: number;
  to: number;
  kind: 'gain' | 'path' | 'loss';
}

/** Cumulative level after each term; terms are positive = loss, negative = gain. */
export function waterfall(terms: BudgetTerm[]): WaterfallStep[] {
  let level = 0;
  return terms.map((term) => {
    const from = level;
    level -= term.valueDb;
    return { term, from, to: level, kind: stepKind(term) };
  });
}

/** Negative terms are gains; free-space path loss is drawn apart from the other losses. */
function stepKind(term: BudgetTerm): WaterfallStep['kind'] {
  if (term.valueDb < 0) return 'gain';
  return term.id === 'fspl' || term.id.startsWith('free-space') ? 'path' : 'loss';
}

/** dB waterfall for any link: every term as a bar, received level, required level. */
@Component({
  selector: 'rf-budget-chart',
  template: `
    @if (view(); as v) {
      <svg [attr.viewBox]="'0 0 ' + v.w + ' ' + v.h" width="100%" role="img" [attr.aria-label]="v.aria">
        <line class="zero" [attr.x1]="v.X0" [attr.x2]="v.X0" y1="10" [attr.y2]="v.axisY" />
        @if (v.reqX !== null) {
          <line class="required" [attr.x1]="v.reqX" [attr.x2]="v.reqX" y1="14" [attr.y2]="v.axisY" />
          <text class="required-label" [attr.x]="v.reqX + 4" y="12">required {{ v.reqText }}</text>
        }
        @for (b of v.bars; track b.id) {
          <rect [attr.class]="'bar ' + b.kind" [attr.x]="b.x" [attr.y]="b.y" [attr.width]="b.width" height="14" rx="2">
            <title>{{ b.title }}</title>
          </rect>
          <text class="bar-label" [class.is-loss]="b.kind === 'loss'" [attr.x]="b.labelX" [attr.y]="b.y + 11" [attr.text-anchor]="b.anchor">{{ b.label }}</text>
        }
        @if (v.received; as r) {
          <circle [attr.class]="'received ' + (r.ok ? 'ok' : 'bad')" [attr.cx]="r.x" [attr.cy]="r.y" r="7"><title>{{ r.title }}</title></circle>
          <text class="received-label" [attr.x]="r.labelX" [attr.y]="r.y + 4" [attr.text-anchor]="r.anchor">Received {{ r.text }}</text>
        }
        <line class="axis" [attr.x1]="v.pad" [attr.x2]="v.w - v.pad" [attr.y1]="v.axisY" [attr.y2]="v.axisY" />
        @for (t of v.ticks; track t.x) {
          <text class="tick" [attr.x]="t.x" [attr.y]="v.axisY + 14" text-anchor="middle">{{ t.label }}</text>
        }
      </svg>
    }
  `,
  styles: `
    :host { display: block; font-family: var(--rf-font-sans); }
    svg { display: block; }
    .bar.gain { fill: var(--rf-ok); }
    .bar.path { fill: #5A6B80; }
    .bar.loss { fill: var(--rf-blocked); }
    .bar-label { font-size: 12px; fill: var(--rf-text-3); font-variant-numeric: tabular-nums; }
    .bar-label.is-loss { fill: var(--rf-blocked-text); }
    .zero { stroke: var(--rf-border-strong); }
    .required { stroke: var(--rf-ok-text); stroke-dasharray: 4 4; stroke-width: 1.5; }
    .required-label { font-size: 11px; fill: var(--rf-ok-text); }
    .received.bad { fill: var(--rf-blocked); }
    .received.ok { fill: var(--rf-ok); }
    .received-label { font-size: 12px; font-weight: 600; fill: var(--rf-text); }
    .axis { stroke: var(--rf-border-strong); }
    .tick { font-family: var(--rf-font-mono); font-size: 11px; fill: #8C98A6; }
  `,
})
export class BudgetChartComponent {
  readonly terms = input<BudgetTerm[] | undefined>([]);
  readonly receivedDbm = input<number | undefined>();
  readonly requiredDbm = input<number | undefined>();
  readonly width = input(656);

  protected readonly view = computed(() => {
    const terms = this.terms() ?? [];
    if (terms.length === 0) return null;
    const steps = waterfall(terms);
    const received = this.receivedDbm() ?? steps.at(-1)!.to;
    const required = this.requiredDbm();
    const w = this.width();
    const pad = 28;
    const rowH = 32;
    const levels = steps.flatMap((s) => [s.from, s.to]).concat([received, required ?? received, 0]);
    const lo = Math.floor((Math.min(...levels) - 10) / 50) * 50;
    const hi = Math.ceil((Math.max(...levels) + 5) / 10) * 10;
    const X = (dbm: number) => pad + ((w - 2 * pad) * (dbm - lo)) / (hi - lo);
    const bars = steps.map((s, i) => {
      const a = X(Math.min(s.from, s.to));
      const b = X(Math.max(s.from, s.to));
      const y = 10 + i * rowH;
      const label = `${s.term.label} ${RfFormat.format(-s.term.valueDb, 'db+', { unit: false })}`;
      const leftRoom = a - pad;
      const anchor: 'start' | 'end' = leftRoom > label.length * 6.6 ? 'end' : 'start';
      return {
        id: s.term.id,
        kind: s.kind,
        x: a,
        y,
        width: Math.max(2, b - a),
        label,
        labelX: anchor === 'end' ? a - 6 : b + 6,
        anchor,
        title: `${s.term.label}: ${RfFormat.full(-s.term.valueDb, 'db+')} · ${s.term.source}`,
      };
    });
    const ry = 10 + steps.length * rowH + 7;
    const rText = RfFormat.format(received, 'dbm', { unit: false });
    const ok = required === undefined || received >= required;
    const reqAria = required !== undefined ? `, required ${RfFormat.format(required, 'dbm')}` : '';
    const ticks = [];
    for (let t = lo; t <= hi; t += 50) ticks.push({ x: X(t), label: RfFormat.format(t, t > 0 ? 'db+' : 'db', { unit: false }).replace('.00', '') });
    return {
      w,
      h: ry + 34,
      pad,
      X0: X(0),
      axisY: ry + 16,
      bars,
      ticks,
      reqX: required !== undefined ? X(required) : null,
      reqText: required !== undefined ? RfFormat.format(required, 'dbm', { unit: false }) : '',
      received: {
        x: X(received),
        y: ry,
        ok,
        text: rText,
        labelX: X(received) + 12,
        anchor: 'start' as const,
        title: RfFormat.full(received, 'dbm'),
      },
      aria: `Link budget waterfall: received ${RfFormat.format(received, 'dbm')}${reqAria}`,
    };
  });
}
