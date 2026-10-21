import { NgTemplateOutlet } from '@angular/common';
import { Component, computed, inject } from '@angular/core';
import { FlightLogRow, ReplayFormat, SegmentOption, SegmentedComponent } from '@rfmap/ui-kit';
import { FlightReplay, ReplayDetail } from '../core/flight-replay.service';
import { ReplayVm } from './replay-vm';

/**
 * The side panel in replay: the flight in one line, then its story (design 03, the default),
 * the numbers at the replay time (design 01) or every flight of the mission (design 10).
 */
@Component({
  selector: 'fpv-replay-panel',
  imports: [SegmentedComponent, NgTemplateOutlet],
  template: `
    @if (replay.view(); as v) {
      <div class="head">
        <b class="craft">{{ title() }}</b>
        <span class="sub">{{ started() }} · {{ firmware() }}</span>
        <span class="line rf-mono">{{ headline() }}</span>
      </div>
    }
    <rf-segmented label="Replay view" [options]="details" [value]="replay.detail()" (valueChange)="setDetail($event)" />
    @if (replay.error(); as e) {
      <div class="rf-card rf-card--blocked note" role="alert">{{ e }}</div>
    }
    @switch (replay.detail()) {
      @case ('story') {
        @if (replay.view(); as v) {
          <div class="section-head">
            <span class="rf-eyebrow">What happened · {{ v.story.length }} {{ v.story.length === 1 ? 'event' : 'events' }}</span>
            @if (replay.attention()) {
              <span class="attn">{{ replay.attention() }} need attention</span>
            }
          </div>
          @if (!v.story.length) {
            <p class="rf-body muted">Nothing to tell: the log holds no flight data.</p>
          }
          <ol class="story">
            @for (e of v.story; track $index) {
              <li>
                <button type="button" class="ev" [class.sel]="replay.selected() === $index" (click)="replay.selectEvent($index)">
                  <span class="n" [class]="'n is-' + e.severity">{{ $index + 1 }}</span>
                  <span class="txt"><b>{{ e.title }}</b>@if (e.detail) {<span>{{ e.detail }}</span>}</span>
                  <span class="at rf-mono">{{ clock(e.t) }}</span>
                </button>
              </li>
            }
          </ol>
          @for (n of v.notes; track n.code) {
            <div class="rf-card note" [class.rf-card--warn]="n.severity !== 'info'">
              <b>{{ n.message }}</b>
              @if (n.fix) {<span>{{ n.fix }}</span>}
            </div>
          }
        } @else {
          <ng-container [ngTemplateOutlet]="empty" />
        }
      }
      @case ('details') {
        @if (replay.view(); as v) {
          <div class="rf-eyebrow">At {{ clock(replay.t()) }} of {{ clock(v.durationS) }}</div>
          <div class="tiles">
            @for (k of readout(); track k.label) {
              <div class="tile" [class.warn]="k.tone === 'warn'">
                <span class="lbl">{{ k.label }}</span>
                <b class="rf-mono">{{ k.value }}</b>
                @if (k.sub) {<span class="lbl">{{ k.sub }}</span>}
              </div>
            }
          </div>
          <div class="rf-eyebrow">Flight summary</div>
          <dl class="kv">
            @for (k of summary(); track k.label) {
              <dt>{{ k.label }}</dt><dd class="rf-mono" [class.warn]="k.tone === 'warn'">{{ k.value }}</dd>
            }
          </dl>
        } @else {
          <ng-container [ngTemplateOutlet]="empty" />
        }
      }
      @case ('logbook') {
        <div class="totals">
          @for (k of totals(); track k.label) {
            <div class="stat" [class.warn]="k.tone === 'warn'"><b class="rf-mono">{{ k.value }}</b><span>{{ k.label }}</span></div>
          }
        </div>
        <div class="section-head">
          <span class="rf-eyebrow">Flights in this mission</span>
          <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="replay.importOpen.set(true)">Add logs</button>
        </div>
        @if (!replay.logbook().length) {
          <p class="rf-body muted">No flights kept yet. Add a Blackbox log to replay it and keep it here.</p>
        }
        <ul class="book">
          @for (r of replay.logbook(); track r.id) {
            <li [class.sel]="r.id === replay.view()?.id">
              <button type="button" class="row" (click)="openRow(r)" [disabled]="!!reason(r)">
                <span class="when">{{ started(r) }}</span>
                <span class="file">{{ r.fileName }}{{ r.index > 0 ? ' · flight ' + (r.index + 1) : '' }}</span>
                @if (reason(r); as why) {
                  <span class="muted">{{ why }}</span>
                } @else {
                  <span class="rf-mono">{{ line(r) }}</span>
                  @if (r.summary.minCellV !== undefined) {<span class="rf-mono">lowest cell {{ r.summary.minCellV.toFixed(2) }} V</span>}
                }
                @if (r.warnings.length) {
                  <span class="warns">@for (w of r.warnings; track $index) {<span class="chip">{{ w }}</span>}</span>
                }
              </button>
              @if (replay.canEdit()) {
                <button type="button" class="rf-icon-btn del" [attr.aria-label]="'Delete ' + r.fileName" (click)="replay.remove(r.id ?? '')">✕</button>
              }
            </li>
          }
        </ul>
      }
    }
    <ng-template #empty>
      <p class="rf-body muted">No flight on screen. Open a flight log to replay it over the plan.</p>
      <button type="button" class="rf-btn rf-btn--primary" (click)="replay.importOpen.set(true)">Load log</button>
    </ng-template>
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 12px; }
    .head { display: flex; flex-direction: column; gap: 2px; }
    .craft { font-size: 18px; }
    .sub { font-size: 12px; color: var(--rf-text-2); }
    .line { font-size: 13px; }
    .muted { color: var(--rf-text-3); }
    .section-head { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
    .attn { font-size: 12px; color: var(--rf-warn-text); font-weight: 600; }
    .note { display: flex; flex-direction: column; gap: 4px; font-size: 12px; }
    ol.story, ul.book { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
    .ev { display: flex; gap: 10px; align-items: flex-start; width: 100%; text-align: left; background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 8px; color: var(--rf-text); cursor: pointer; font: inherit; }
    .ev:hover { background: var(--rf-surface-1); }
    .ev.sel { background: var(--rf-surface-3, #1e2935); border-color: var(--rf-border-strong); }
    .n { flex: none; width: 22px; height: 22px; border-radius: 6px; display: flex; align-items: center; justify-content: center; font-size: 12px; font-weight: 700; color: #0b0f14; background: var(--rf-video); }
    .n.is-warn { background: var(--rf-warn); }
    .n.is-blocked { background: var(--rf-blocked); }
    .txt { flex: 1; display: flex; flex-direction: column; gap: 2px; font-size: 13px; }
    .txt span { color: var(--rf-text-2); font-size: 12px; }
    .at { font-size: 12px; color: var(--rf-text-2); }
    .tiles { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
    .tile { padding: 8px 10px; border-radius: var(--rf-radius-md); background: var(--rf-surface-1); border: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 2px; }
    .tile b { font-size: 18px; }
    .tile.warn b, dd.warn, .stat.warn b { color: var(--rf-warn-text); }
    .lbl { font-size: 11px; color: var(--rf-text-2); }
    .kv { display: grid; grid-template-columns: auto 1fr; gap: 6px 12px; margin: 0; font-size: 13px; }
    .kv dt { color: var(--rf-text-2); }
    .kv dd { margin: 0; text-align: right; }
    .totals { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
    .stat { display: flex; flex-direction: column; }
    .stat b { font-size: 22px; }
    .stat span { font-size: 12px; color: var(--rf-text-2); }
    .book li { display: flex; gap: 4px; align-items: flex-start; border-radius: 10px; }
    .book li.sel { background: var(--rf-surface-1); }
    .row { flex: 1; display: flex; flex-direction: column; gap: 2px; text-align: left; background: transparent; border: 0; padding: 8px; color: var(--rf-text); font: inherit; font-size: 12px; cursor: pointer; border-radius: 10px; }
    .row:disabled { cursor: default; }
    .when { font-weight: 600; font-size: 13px; }
    .file { color: var(--rf-text-2); }
    .warns { display: flex; flex-wrap: wrap; gap: 4px; }
    .chip { padding: 1px 6px; border-radius: 999px; background: var(--rf-warn-bg); color: var(--rf-warn-text); border: 1px solid var(--rf-warn-border); font-size: 11px; }
    .del { margin-top: 6px; }
  `,
})
export class ReplayPanelComponent {
  protected readonly replay = inject(FlightReplay);
  protected readonly details: SegmentOption<ReplayDetail>[] = [
    { value: 'story', label: 'Story' },
    { value: 'details', label: 'Details' },
    { value: 'logbook', label: 'Logbook' },
  ];
  protected readonly title = computed(() => {
    const v = this.replay.view();
    return v ? ReplayVm.title(v) : '';
  });
  protected readonly firmware = computed(() => {
    const v = this.replay.view();
    return v ? ReplayVm.firmware(v) : '';
  });
  protected readonly headline = computed(() => {
    const v = this.replay.view();
    return v ? ReplayVm.headline(v) : '';
  });
  protected readonly summary = computed(() => {
    const v = this.replay.view();
    return v ? ReplayVm.summary(v) : [];
  });
  protected readonly readout = computed(() => {
    const v = this.replay.view();
    const r = this.replay.reading();
    return v && r ? ReplayVm.readout(v, r) : [];
  });
  protected readonly totals = computed(() => ReplayVm.totals(this.replay.logbook()));

  protected started(row?: FlightLogRow): string {
    return ReplayFormat.started((row ?? this.replay.view())?.startedAt);
  }

  protected clock(t: number): string {
    return ReplayFormat.clock(t);
  }

  protected line(r: FlightLogRow): string {
    return ReplayVm.headline(r);
  }

  protected reason(r: FlightLogRow): string | null {
    return ReplayVm.emptyReason(r);
  }

  protected setDetail(d: ReplayDetail): void {
    this.replay.detail.set(d);
    if (d === 'logbook') void this.replay.refresh();
  }

  protected openRow(r: FlightLogRow): void {
    if (r.id) void this.replay.open(r.id);
  }
}
