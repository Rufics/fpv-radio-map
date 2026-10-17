import { Component, computed, inject, input, signal } from '@angular/core';
import { MissionStore, SegmentedComponent } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';

interface DayCell {
  date: Date;
  label: string;
  tier: 'hourly' | 'long-range' | 'none';
  selected: boolean;
  today: boolean;
  iso: string;
}

/** Mission time: Now (default) or a planned date/time, with the forecast horizon tiers. */
@Component({
  selector: 'fpv-mission-time',
  imports: [SegmentedComponent],
  template: `
    <div class="rf-eyebrow">Mission time</div>
    <rf-segmented label="Mission time" [options]="modes" [value]="mode()" (valueChange)="setMode($event)" />
    @if (mode() === 'planned') {
      <div class="row">
        <label class="rf-field grow">Date<input class="rf-input rf-mono" type="date" [value]="dateValue()" (change)="setDate($any($event.target).value)" [disabled]="!editable()" /></label>
        <label class="rf-field">Time<input class="rf-input rf-mono" type="time" [value]="timeValue()" (change)="setTime($any($event.target).value)" [disabled]="!editable()" /></label>
      </div>
      <div class="rf-caption">local time · {{ relative() }}</div>
      @if (days().length) {
        <div class="tiers" aria-hidden="true">
          <span class="t hourly" [style.grid-column]="'span ' + counts().hourly">Hourly · {{ counts().hourly }} d</span>
          @if (counts().long) {<span class="t long" [style.grid-column]="'span ' + counts().long">Long-range · to {{ counts().hourly + counts().long }} d</span>}
          @if (counts().none) {<span class="t none" [style.grid-column]="'span ' + counts().none">None</span>}
        </div>
        <div class="days" role="group" aria-label="Pick a day">
          @for (d of days(); track d.iso) {
            <button type="button" class="day" [class]="'day ' + d.tier" [class.sel]="d.selected" [class.today]="d.today" [attr.aria-pressed]="d.selected" [attr.aria-label]="d.date.toDateString() + ', ' + tierText(d.tier)" (click)="pickDay(d)" [disabled]="!editable()">{{ d.label }}</button>
          }
        </div>
      }
      <div class="note" [class]="'note ' + tier()">
        @switch (tier()) {
          @case ('hourly') {<b>Within the hourly forecast.</b> Wind, temperature, pressure and humidity per hour feed air density, refraction and battery.}
          @case ('long-range') {<b>Long-range forecast.</b> Coarser models; every weather-based result carries “long-range forecast”.}
          @default {<b>Beyond the forecast horizon.</b> No forecast exists. The mission uses the standard atmosphere and says so; we do not invent weather.}
        }
      </div>
      <div class="rf-caption">Cut-off dates come from the forecast run the server holds{{ runText() }}.</div>
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 8px; }
    .row { display: flex; gap: 8px; }
    .grow { flex: 1; }
    .tiers, .days { display: grid; grid-template-columns: repeat(21, minmax(0, 1fr)); gap: 2px; }
    .t { padding: 3px 0; text-align: center; font-size: 10px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .t.hourly { background: var(--rf-ok-bg); color: var(--rf-ok-text); border-radius: 6px 0 0 6px; }
    .t.long { background: var(--rf-warn-bg); color: var(--rf-warn-text); }
    .t.none { background: var(--rf-surface-3); color: var(--rf-text-2); border-radius: 0 6px 6px 0; }
    .day { height: 32px; padding: 0; border: 0; border-radius: 4px; background: transparent; color: var(--rf-text-2); font-family: var(--rf-font-mono); font-size: 11px; }
    .day.none { color: var(--rf-text-disabled); }
    .day.today { background: var(--rf-selected); color: var(--rf-text); }
    .day.sel { background: var(--rf-ok); color: var(--rf-ok-ink); font-weight: 700; }
    .note { padding: 8px 10px; border-radius: 10px; font-size: 12px; line-height: 1.45; color: var(--rf-text-3); }
    .note.hourly { background: var(--rf-ok-bg); } .note.hourly b { color: var(--rf-ok-text); }
    .note.long-range { background: var(--rf-warn-bg); } .note.long-range b { color: var(--rf-warn-text); }
    .note.none { background: var(--rf-surface-3); } .note.none b { color: var(--rf-text); }
  `,
})
export class MissionTimeComponent {
  private readonly store = inject(MissionStore);
  private readonly data = inject(FpvDataService);
  readonly editable = input(true);

  protected readonly modes = [
    { value: 'now' as const, label: 'Now' },
    { value: 'planned' as const, label: 'Planned' },
  ];
  protected readonly mode = computed(() => this.store.mission()?.time.mode ?? 'now');
  private readonly when = computed(() => {
    const t = this.store.mission()?.time;
    return t?.mode === 'planned' && t.utc ? new Date(t.utc) : new Date();
  });
  protected readonly tier = computed(() => this.data.tierFor(this.mode() === 'planned' ? this.when().toISOString() : null));

  protected readonly dateValue = computed(() => {
    const d = this.when();
    return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
  });
  protected readonly timeValue = computed(() => {
    const d = this.when();
    return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  });
  protected readonly relative = computed(() => {
    const days = Math.round((this.startOfDay(this.when()) - this.startOfDay(new Date())) / 86400e3);
    return days === 0 ? 'today' : days === 1 ? 'tomorrow' : days > 0 ? `in ${days} days` : `${-days} days ago`;
  });
  protected readonly runText = computed(() => {
    const r = this.data.horizon()?.runUtc;
    return r ? ` (run ${new Date(r).toLocaleString('en-GB', { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' })})` : '';
  });

  protected readonly days = computed<DayCell[]>(() => {
    if (!this.data.horizon()) return [];
    const out: DayCell[] = [];
    const today = new Date();
    today.setHours(12, 0, 0, 0);
    const sel = this.startOfDay(this.when());
    for (let i = 0; i < 21; i++) {
      const d = new Date(today.getTime() + i * 86400e3);
      out.push({ date: d, label: String(d.getDate()), tier: this.data.tierFor(d.toISOString()), selected: this.startOfDay(d) === sel, today: i === 0, iso: d.toISOString() });
    }
    return out;
  });
  protected readonly counts = computed(() => {
    const d = this.days();
    return { hourly: d.filter((x) => x.tier === 'hourly').length, long: d.filter((x) => x.tier === 'long-range').length, none: d.filter((x) => x.tier === 'none').length };
  });

  protected tierText(t: string): string {
    return t === 'hourly' ? 'hourly forecast' : t === 'long-range' ? 'long-range forecast' : 'no forecast';
  }

  private startOfDay(d: Date): number {
    const c = new Date(d);
    c.setHours(0, 0, 0, 0);
    return c.getTime();
  }

  protected setMode(mode: 'now' | 'planned'): void {
    if (!this.editable()) return;
    if (mode === 'now') this.store.update((m) => ({ ...m, time: { mode: 'now' } }));
    else {
      const d = new Date(Date.now() + 86400e3);
      d.setMinutes(0, 0, 0);
      this.store.update((m) => ({ ...m, time: { mode: 'planned', utc: d.toISOString() } }));
    }
  }

  private setWhen(d: Date): void {
    if (Number.isNaN(d.getTime())) return;
    this.store.update((m) => ({ ...m, time: { mode: 'planned', utc: d.toISOString() } }));
  }

  protected setDate(v: string): void {
    const [y, mo, da] = v.split('-').map(Number);
    const d = new Date(this.when());
    d.setFullYear(y, mo - 1, da);
    this.setWhen(d);
  }

  protected setTime(v: string): void {
    const [h, mi] = v.split(':').map(Number);
    const d = new Date(this.when());
    d.setHours(h, mi, 0, 0);
    this.setWhen(d);
  }

  protected pickDay(cell: DayCell): void {
    const d = new Date(this.when());
    d.setFullYear(cell.date.getFullYear(), cell.date.getMonth(), cell.date.getDate());
    this.setWhen(d);
  }

  readonly selectedHint = signal('');
}
