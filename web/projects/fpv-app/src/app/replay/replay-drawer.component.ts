import { Component, computed, inject } from '@angular/core';
import { FlightChannel, ReplayFormat, SegmentOption, SegmentedComponent, TrackMetric, TrackScale } from '@rfmap/ui-kit';
import { FlightReplay, REPLAY_SPEEDS } from '../core/flight-replay.service';
import { CHART_W, ChartRowVm, ReplayChart } from './replay-chart';

interface RowSpec {
  key: string;
  label: string;
  channel: FlightChannel | 'offRoute';
  range?: [number, number];
  guide?: number;
  guideLabel?: string;
  format: (v: number) => string;
}

const CHART_H = 30;

/**
 * The replay clock under the map: play, speed, track colour, the scrubber with the flight-mode
 * bands and the story marks, and in the detailed view (design 01) or against the plan (design 08)
 * the charts of the flight on the same time axis.
 */
@Component({
  selector: 'fpv-replay-drawer',
  imports: [SegmentedComponent],
  template: `
    @if (replay.view(); as v) {
      <div class="controls">
        <button type="button" class="rf-btn rf-btn--sm rf-btn--primary play" (click)="replay.toggle()" [attr.aria-label]="replay.playing() ? 'Pause' : 'Play'">
          {{ replay.playing() ? '❚❚' : '▶' }}
        </button>
        <span class="clock rf-mono"><b>{{ clock(replay.t()) }}</b> / {{ clock(end()) }}</span>
        <div class="speeds" role="group" aria-label="Replay speed">
          @for (s of speeds; track s) {
            <button type="button" [attr.aria-pressed]="replay.speed() === s" (click)="replay.speed.set(s)">{{ s }}×</button>
          }
        </div>
        @if (colourOptions().length > 1) {
          <span class="lbl">Colour track by</span>
          <rf-segmented label="Colour track by" [options]="colourOptions()" [value]="replay.colourBy()" (valueChange)="replay.colourBy.set($event)" />
        }
        @if (replay.selectedEvent(); as e) {
          <div class="event" [class]="'event is-' + e.severity">
            <b>{{ (replay.selected() ?? 0) + 1 }} · {{ e.title }}</b>
            <span class="rf-mono">{{ clock(e.t) }}</span>
            <button type="button" class="rf-btn rf-btn--sm rf-btn--muted" (click)="replay.step(-1)" aria-label="Previous event">‹</button>
            <button type="button" class="rf-btn rf-btn--sm rf-btn--muted" (click)="replay.step(1)" aria-label="Next event">›</button>
            <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="playFrom()">Play from here</button>
          </div>
        }
      </div>
      <svg class="scrub" [attr.viewBox]="'0 0 ' + width + ' 30'" preserveAspectRatio="none" role="slider" tabindex="0"
           aria-label="Replay time" [attr.aria-valuemin]="0" [attr.aria-valuemax]="end()" [attr.aria-valuenow]="replay.t()" [attr.aria-valuetext]="clock(replay.t())"
           (pointerdown)="seekAt($event)" (pointermove)="drag($event)" (keydown)="key($event)">
        @for (b of bands(); track $index) {
          <rect [attr.x]="b.x" y="8" [attr.width]="b.w" height="14" [attr.fill]="b.color"><title>{{ b.label }}</title></rect>
        }
        @for (k of ticks(); track $index) {
          <rect [attr.x]="k.x - 2" y="0" width="4" height="30" [attr.class]="'tick is-' + k.severity"><title>{{ k.title }}</title></rect>
        }
        <rect class="cursor" [attr.x]="cursorX() - 1.5" y="0" width="3" height="30" />
      </svg>
      <div class="band-keys">
        @for (b of bandKeys(); track b.label) {
          <span><i [style.background]="b.color"></i>{{ b.label }}</span>
        }
      </div>
      @if (chartsShown()) {
        <div class="charts">
          @for (r of rows(); track r.key) {
            <div class="chart">
              <span class="lbl">{{ r.label }}</span>
              <svg [attr.viewBox]="'0 0 ' + width + ' ' + chartH" preserveAspectRatio="none" (pointerdown)="seekAt($event)" (pointermove)="drag($event)" aria-hidden="true">
                @if (r.guideY !== undefined) {
                  <line x1="0" [attr.x2]="width" [attr.y1]="r.guideY * chartH" [attr.y2]="r.guideY * chartH" class="guide" />
                }
                <path [attr.d]="r.path" class="line" vector-effect="non-scaling-stroke" />
                <rect class="cursor" [attr.x]="cursorX() - 1.5" y="0" width="3" [attr.height]="chartH" />
              </svg>
              <span class="range rf-mono"><span>{{ r.top }}</span><span>{{ r.bottom }}</span></span>
            </div>
          }
        </div>
      }
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 8px; padding: 12px 16px; background: rgba(16, 22, 29, 0.97); border-top: 1px solid var(--rf-border); }
    .controls { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
    .play { width: 40px; }
    .clock { font-size: 13px; color: var(--rf-text-2); }
    .clock b { color: var(--rf-text); font-size: 15px; }
    .speeds { display: inline-flex; border: 1px solid var(--rf-border); border-radius: 8px; overflow: hidden; }
    .speeds button { border: 0; background: transparent; color: var(--rf-text-2); padding: 4px 8px; font: inherit; font-size: 12px; cursor: pointer; }
    .speeds button[aria-pressed='true'] { background: var(--rf-surface-3, #1e2935); color: var(--rf-text); font-weight: 600; }
    .lbl { font-size: 11px; color: var(--rf-text-2); }
    .event { margin-left: auto; display: flex; align-items: center; gap: 8px; font-size: 13px; padding: 4px 4px 4px 10px; border-radius: 8px; border: 1px solid var(--rf-border); }
    .event.is-warn { border-color: var(--rf-warn-border); }
    .event.is-blocked { border-color: var(--rf-blocked-border); }
    svg { display: block; width: 100%; cursor: pointer; touch-action: none; }
    .scrub { height: 30px; }
    .tick { fill: var(--rf-video); }
    .tick.is-warn { fill: var(--rf-warn); }
    .tick.is-blocked { fill: var(--rf-blocked); }
    .cursor { fill: var(--rf-text); }
    .band-keys { display: flex; gap: 12px; font-size: 11px; color: var(--rf-text-2); }
    .band-keys span { display: inline-flex; align-items: center; gap: 4px; }
    .band-keys i { width: 10px; height: 10px; border-radius: 2px; }
    .charts { display: flex; flex-direction: column; gap: 4px; }
    .chart { display: grid; grid-template-columns: 90px 1fr 64px; align-items: center; gap: 8px; }
    .chart svg { height: 30px; background: var(--rf-surface-1); border-radius: 4px; }
    .line { fill: none; stroke: var(--rf-ok); stroke-width: 1.5; }
    .guide { stroke: var(--rf-warn); stroke-dasharray: 4 4; stroke-width: 1; vector-effect: non-scaling-stroke; }
    .range { display: flex; flex-direction: column; justify-content: space-between; height: 30px; font-size: 10px; color: var(--rf-text-3); }
  `,
})
export class ReplayDrawerComponent {
  protected readonly replay = inject(FlightReplay);
  protected readonly speeds = REPLAY_SPEEDS;
  protected readonly width = CHART_W;
  protected readonly chartH = CHART_H;

  protected readonly end = computed(() => this.replay.timeline()?.end ?? 0);
  private readonly chart = computed(() => {
    const tl = this.replay.timeline();
    return new ReplayChart(tl?.start ?? 0, tl?.end ?? 0);
  });
  protected readonly cursorX = computed(() => this.chart().x(this.replay.t()));
  protected readonly bands = computed(() => this.chart().bands(this.replay.view()?.bands ?? []));
  protected readonly bandKeys = computed(() => {
    const seen = new Map<string, string>();
    for (const b of this.bands()) seen.set(b.label, b.color);
    return [...seen].map(([label, color]) => ({ label, color }));
  });
  protected readonly ticks = computed(() => this.chart().ticks(this.replay.view()?.story ?? []));
  protected readonly colourOptions = computed<SegmentOption<TrackMetric>[]>(() =>
    this.replay.metrics().map((m) => ({ value: m, label: TrackScale.title(m).replace('Ground speed', 'Speed').replace('GPS satellites', 'GPS').replace('Cell voltage', 'Battery') })),
  );
  protected readonly chartsShown = computed(() => this.replay.detail() === 'details' || this.replay.mode() === 'plan-fact');

  protected readonly rows = computed<ChartRowVm[]>(() => {
    const v = this.replay.view();
    if (!v) return [];
    const link: FlightChannel = v.timeline.channels.linkQualityPct ? 'linkQualityPct' : 'rssiPct';
    const specs: RowSpec[] = [
      { key: 'alt', label: 'Altitude', channel: 'altM', format: (x) => `${Math.round(x)} m` },
      { key: 'speed', label: 'Speed', channel: 'speedMps', format: (x) => ReplayFormat.speed(x) },
      { key: 'cell', label: 'Cell V', channel: 'cellV', guide: 3.5, guideLabel: '3.50 V', format: (x) => `${x.toFixed(2)} V` },
      { key: 'link', label: link === 'linkQualityPct' ? 'Link quality' : 'RSSI', channel: link, range: [0, 100], guide: 30, format: (x) => `${Math.round(x)} %` },
    ];
    if (this.replay.mode() === 'plan-fact' && v.plan) specs.push({ key: 'off', label: 'Off route', channel: 'offRoute', format: (x) => ReplayFormat.distance(x) });
    return specs.flatMap((s) => this.row(s));
  });

  private row(s: RowSpec): ChartRowVm[] {
    const v = this.replay.view();
    if (!v) return [];
    const values = s.channel === 'offRoute' ? (v.plan?.offRouteM ?? []) : (v.timeline.channels[s.channel] ?? []);
    if (!values.some((x) => x !== null)) return [];
    const p = this.chart().path(v.timeline.t, values, CHART_H, s.range);
    const row: ChartRowVm = { key: s.key, label: s.label, path: p.d, top: s.format(p.max), bottom: s.format(p.min) };
    if (s.guide !== undefined && p.max > p.min && s.guide >= p.min && s.guide <= p.max) {
      row.guideY = 1 - (s.guide - p.min) / (p.max - p.min);
      row.guideLabel = s.guideLabel;
    }
    return [row];
  }

  protected clock(t: number): string {
    return ReplayFormat.clock(t);
  }

  protected seekAt(e: PointerEvent): void {
    const el = e.currentTarget as SVGElement;
    const box = el.getBoundingClientRect();
    if (box.width <= 0) return;
    this.replay.seek(this.chart().timeAt((e.clientX - box.left) / box.width));
  }

  protected drag(e: PointerEvent): void {
    if (e.buttons === 1) this.seekAt(e);
  }

  protected key(e: KeyboardEvent): void {
    const by = e.shiftKey ? 10 : 1;
    if (e.key === 'ArrowRight') this.replay.seek(this.replay.t() + by);
    else if (e.key === 'ArrowLeft') this.replay.seek(this.replay.t() - by);
    else if (e.key === ' ') this.replay.toggle();
    else return;
    e.preventDefault();
  }

  protected playFrom(): void {
    this.replay.pause();
    this.replay.play();
  }
}
