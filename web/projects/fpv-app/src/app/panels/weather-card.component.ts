import { Component, computed, inject, input } from '@angular/core';
import { RfFormat } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { FALLBACK_SPEED_MPS, RouteWeatherService } from '../core/route-weather.service';
import { RouteColoursComponent } from './route-colours.component';

/** Side panel card: the weather at each route point at its arrival time, and what changes along the route. */
@Component({
  selector: 'fpv-weather-card',
  imports: [RouteColoursComponent],
  template: `
    <section class="card" aria-labelledby="wx-card-title">
      <div class="head">
        <span id="wx-card-title" class="rf-eyebrow">Weather on the route</span>
        <span class="window rf-mono">{{ wx.window() }}</span>
      </div>
      @if (routeColours()) {
        <fpv-route-colours />
      }
      @switch (wx.status()) {
        @case ('loading') {
          <div class="rf-caption" role="status">Loading the forecast along the route…</div>
        }
        @case ('error') {
          <div class="msg bad" role="alert">
            The forecast could not be loaded: {{ wx.error() }}
            <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" (click)="wx.refresh()">Try again</button>
          </div>
        }
        @case ('ready') {
          @if (wx.beyondForecast()) {
            <div class="msg">No forecast covers {{ wx.window() }} yet, so there is no weather to show. Pick a time within the forecast horizon.</div>
          } @else {
            @if (launch(); as l) {
              <div class="now">
                <span class="temp rf-mono">{{ l.temperature }}</span>
                <span class="now-text">
                  <span>At {{ l.name }}, {{ l.eta }}</span>
                  <span class="muted">
                    @if (l.windArrowDeg !== null) {<span class="arrow" aria-hidden="true" [style.transform]="'rotate(' + l.windArrowDeg + 'deg)'">↑</span>}
                    {{ l.windSpeed }} from {{ l.windCompass }}{{ humidity() }}
                  </span>
                </span>
              </div>
            }
            <table class="points rf-mono">
              <caption class="sr-only">Weather at each route point at its arrival time</caption>
              <thead>
                <tr><th scope="col">Point</th><th scope="col">ETA</th><th scope="col">Wind m/s</th><th scope="col">Temp</th><th scope="col">Rain</th></tr>
              </thead>
              <tbody>
                @for (k of wx.keyPoints(); track $index) {
                  <tr>
                    <th scope="row">{{ k.name }}{{ $last && $index > 0 && isReturn() ? ' ↩' : '' }}</th>
                    <td>{{ k.eta }}</td>
                    <td [class]="'lvl-' + k.windLevel" [attr.aria-label]="k.windSpeed + (k.windCompass ? ' from ' + k.windCompass : '') + (k.alongTrack ? ', ' + k.alongTrack : '')" [title]="k.alongTrack">
                      @if (k.windArrowDeg !== null) {<span class="arrow" aria-hidden="true" [style.transform]="'rotate(' + k.windArrowDeg + 'deg)'">↑</span>}
                      {{ short(k.windSpeed) }}
                    </td>
                    <td>{{ k.temperature }}</td>
                    <td [class]="'lvl-' + k.rainLevel">{{ k.rain }}</td>
                  </tr>
                }
              </tbody>
            </table>
            <ul class="summary">
              @for (s of wx.summary(); track s.key) {
                <li [class]="'lvl-' + s.level">{{ s.text }}</li>
              }
            </ul>
            @if (wx.source(); as src) {
              <div class="rf-caption">
                {{ src.model }}{{ src.run ? ', run ' + src.run : '' }}{{ src.tier === 'long-range' ? ' · long-range forecast' : '' }}
                @if (src.stale) {<b class="warn"> · forecast run is old</b>}
              </div>
            }
          }
        }
      }
      <div class="rf-caption">{{ speedText() }}</div>
      @if (!wx.usedInCalculation()) {
        <div class="rf-caption">Weather is off for this mission's calculation; results use the standard atmosphere.</div>
      }
      <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="ws.briefingOpen.set(true)" [disabled]="wx.status() !== 'ready' || wx.beyondForecast()">Weather briefing</button>
    </section>
  `,
  styles: `
    :host { display: block; flex: none; }
    .card { display: flex; flex-direction: column; gap: 10px; padding: 12px; border-radius: var(--rf-radius-lg); background: var(--rf-surface-1); border: 1px solid var(--rf-border); }
    .head { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; }
    .window { font-size: 12px; color: var(--rf-text-2); }
    .now { display: flex; align-items: center; gap: 12px; }
    .temp { font-size: 28px; font-weight: 500; }
    .now-text { display: flex; flex-direction: column; gap: 2px; font-size: 13px; }
    .muted { color: var(--rf-text-2); }
    .arrow { display: inline-block; width: 12px; text-align: center; font-family: var(--rf-font-sans); }
    .points { width: 100%; border-collapse: collapse; font-size: 12px; }
    .points th, .points td { padding: 6px 4px; text-align: right; border-bottom: 1px solid var(--rf-divider); white-space: nowrap; font-weight: 400; }
    .points thead th { font-family: var(--rf-font-sans); font-size: 11px; color: var(--rf-text-2); text-transform: uppercase; letter-spacing: 0.06em; }
    .points th:first-child { text-align: left; }
    .points td.lvl-notable { color: var(--rf-warn-text); }
    .points td.lvl-strong { color: var(--rf-blocked-text); font-weight: 600; }
    .summary { margin: 0; padding: 0; list-style: none; display: flex; flex-direction: column; gap: 6px; font-size: 13px; }
    .summary li { padding: 6px 10px; border-radius: var(--rf-radius-sm); background: var(--rf-surface-3); border: 1px solid var(--rf-border); }
    .summary li.lvl-notable { background: var(--rf-warn-bg); border-color: var(--rf-warn-border); color: var(--rf-warn-text); }
    .summary li.lvl-strong { background: var(--rf-blocked-bg); border-color: var(--rf-blocked-border); color: var(--rf-blocked-text); }
    .msg { font-size: 13px; color: var(--rf-text-3); display: flex; flex-direction: column; gap: 8px; align-items: flex-start; }
    .msg.bad { color: var(--rf-blocked-text); }
    .warn { color: var(--rf-warn-text); font-weight: 600; }
    .sr-only { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
  `,
})
export class WeatherCardComponent {
  protected readonly ws = inject(FpvWorkspace);
  protected readonly wx = inject(RouteWeatherService);
  /** Phone: the route colour picker and legend sit here instead of over the small map */
  readonly routeColours = input(false);
  protected readonly launch = computed(() => this.wx.keyPoints()[0] ?? null);
  protected readonly isReturn = computed(() => this.ws.mission()?.returnToPilot !== false);

  protected readonly humidity = computed(() => {
    const rh = this.launch()?.reading?.relativeHumidity;
    return rh === undefined ? '' : ` · ${Math.round(rh)}% RH`;
  });

  protected readonly speedText = computed(() => {
    const d = this.wx.drone();
    if (d) return `Arrival times at the cruise speed of the ${d.name}, ${RfFormat.format(d.cruiseAirspeedMps, 'mps')}; wind is not applied.`;
    return `Arrival times at an assumed ${RfFormat.format(FALLBACK_SPEED_MPS, 'mps')}: the mission names no drone from the library. Wind is not applied.`;
  });

  /** "7.4 m/s" → "7.4" for the table, whose header carries the unit. */
  protected short(text: string): string {
    return text.replace(' m/s', '');
  }
}
