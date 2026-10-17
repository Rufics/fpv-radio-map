import { Component, computed, input, model, output } from '@angular/core';
import { Job, JobPart } from '../models/api.models';
import { RfFormat } from '../format/rf-format';

export type Tone = 'ok' | 'blocked' | 'warn' | 'off' | 'info';

/** Status pill. Content must carry a letter and a mark (✓ ✕ !), never colour alone. */
@Component({
  selector: 'rf-pill',
  template: `<ng-content />`,
  host: { '[class]': "'rf-pill rf-pill--' + tone() + (small() ? ' rf-pill--sm' : '')", role: 'status' },
})
export class PillComponent {
  readonly tone = input<Tone>('off');
  readonly small = input(false);
}

export interface SegmentOption<T extends string> {
  value: T;
  label: string;
}

/** Segmented control: real buttons with aria-pressed. */
@Component({
  selector: 'rf-segmented',
  template: `
    <div class="rf-segmented" role="group" [attr.aria-label]="label()">
      @for (o of options(); track o.value) {
        <button type="button" [attr.aria-pressed]="o.value === value()" (click)="value.set(o.value)">{{ o.label }}</button>
      }
    </div>
  `,
})
export class SegmentedComponent<T extends string> {
  readonly options = input.required<SegmentOption<T>[]>();
  readonly value = model.required<T>();
  readonly label = input('Options');
}

/** Number input with − / + steppers and a unit (Components.dc.html "Inputs with units"). */
@Component({
  selector: 'rf-stepper',
  template: `
    <label class="rf-field">
      {{ label() }}
      <span class="box" [class.dense]="dense()">
        <button type="button" [attr.aria-label]="'Decrease ' + label()" (click)="bump(-1)" [disabled]="value() <= min()">−</button>
        <span class="val">
          <input type="number" [min]="min()" [max]="max()" [step]="step()" [value]="value()" (change)="onInput($event)" [attr.aria-label]="label()" />
          <span class="unit">{{ unit() }}</span>
        </span>
        <button type="button" [attr.aria-label]="'Increase ' + label()" (click)="bump(1)" [disabled]="value() >= max()">+</button>
      </span>
      @if (hint()) {
        <span class="rf-caption">{{ hint() }}</span>
      }
    </label>
  `,
  styles: `
    .box { display: flex; height: 44px; border: 1px solid var(--rf-border); border-radius: var(--rf-radius-sm); background: var(--rf-bg); overflow: hidden; }
    .box.dense { height: 32px; }
    .box > button { width: 44px; border: 0; background: transparent; color: var(--rf-text); font-size: 18px; }
    .box.dense > button { width: 32px; }
    .box > button:first-child { border-right: 1px solid var(--rf-border); }
    .box > button:last-child { border-left: 1px solid var(--rf-border); }
    .box > button:disabled { color: var(--rf-text-disabled); }
    .val { flex: 1; display: flex; align-items: center; justify-content: center; gap: 6px; font-family: var(--rf-font-mono); font-size: 15px; color: var(--rf-text); }
    input { width: 64px; background: transparent; border: 0; text-align: right; font-family: var(--rf-font-mono); font-size: 15px; -moz-appearance: textfield; }
    input::-webkit-inner-spin-button { display: none; }
    .unit { color: var(--rf-text-2); }
  `,
})
export class StepperComponent {
  readonly label = input.required<string>();
  readonly value = model.required<number>();
  readonly unit = input('m');
  readonly min = input(0);
  readonly max = input(10000);
  readonly step = input(1);
  readonly hint = input<string>('');
  readonly dense = input(false);

  protected bump(dir: number): void {
    this.set(this.value() + dir * this.step());
  }
  protected onInput(e: Event): void {
    const v = Number((e.target as HTMLInputElement).value);
    if (Number.isFinite(v)) this.set(v);
  }
  private set(v: number): void {
    this.value.set(Math.min(this.max(), Math.max(this.min(), v)));
  }
}

const PART_MARK: Record<string, string> = { done: '✓', cached: '✓', running: '●', waiting: '○', failed: '✕' };

/** Job progress with parts (States.dc.html "Long calculation"). */
@Component({
  selector: 'rf-job-progress',
  template: `
    <div class="parts" role="status" aria-live="polite">
      @for (p of parts(); track $index) {
        <div class="part" [class]="'is-' + (p.state ?? 'waiting')">
          <div class="row">
            <span><span class="mark" aria-hidden="true">{{ mark(p) }}</span> {{ p.name }}</span>
            <span class="state">{{ stateText(p) }}</span>
          </div>
          @if (p.state === 'running') {
            <div class="bar" role="progressbar" [attr.aria-valuenow]="pct(p)" aria-valuemin="0" aria-valuemax="100" [attr.aria-label]="p.name">
              <div [style.width.%]="pct(p)"></div>
            </div>
          }
        </div>
      }
      @if (parts().length === 0) {
        <div class="part is-running"><div class="row"><span><span class="mark">●</span> Starting</span><span class="state">queued</span></div></div>
      }
    </div>
    @if (showCancel()) {
      <button type="button" class="rf-btn rf-btn--secondary" (click)="cancel.emit()">Cancel</button>
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 10px; padding: 12px; border-radius: var(--rf-radius-lg); background: var(--rf-surface-3); }
    .parts { display: flex; flex-direction: column; gap: 8px; }
    .row { display: flex; justify-content: space-between; gap: 8px; }
    .state { font-size: 12px; color: var(--rf-text-2); font-family: var(--rf-font-mono); }
    .is-done .mark, .is-cached .mark { color: var(--rf-ok-text); }
    .is-running .mark { color: var(--rf-warn-text); }
    .is-waiting { color: var(--rf-text-2); }
    .is-failed .mark { color: var(--rf-blocked-text); }
    .bar { margin-top: 6px; height: 8px; border-radius: 4px; background: var(--rf-bg); overflow: hidden; }
    .bar > div { height: 100%; background: var(--rf-ok); transition: width 0.3s; }
  `,
})
export class JobProgressComponent {
  readonly job = input<Job | null>(null);
  readonly extraParts = input<JobPart[]>([]);
  readonly showCancel = input(true);
  readonly cancel = output<void>();
  protected readonly parts = computed(() => [...this.extraParts(), ...(this.job()?.parts ?? [])]);
  protected mark(p: JobPart): string {
    return PART_MARK[p.state ?? 'waiting'] ?? '○';
  }
  protected pct(p: JobPart): number {
    return Math.round((p.progress ?? 0) * 100);
  }
  protected stateText(p: JobPart): string {
    if (p.state === 'running') return RfFormat.format(p.progress ?? 0, 'pct');
    if (p.state === 'cached') return 'from cache';
    return p.state ?? 'waiting';
  }
}

/** Heat scale legend: +30 … 0 dB margin. */
@Component({
  selector: 'rf-heat-legend',
  template: `
    <div class="bar" aria-hidden="true"></div>
    <div class="ticks rf-mono"><span>+30 dB</span><span>+20</span><span>+10</span><span>0 dB margin</span></div>
    <span class="sr-only">Signal margin scale from +30 dB, light yellow, to 0 dB, dark purple. Below 0 dB is not coloured.</span>
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 4px; font-size: 11px; color: var(--rf-text-2); }
    .bar { height: 10px; border-radius: 5px; background: var(--rf-heat-gradient); }
    .ticks { display: flex; justify-content: space-between; }
  `,
})
export class HeatLegendComponent {}
