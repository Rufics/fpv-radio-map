import { Component, computed, inject, input } from '@angular/core';
import { SegmentedComponent, WeatherScale } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { RouteWeatherService } from '../core/route-weather.service';

/** Weather mode: which value colours the route, and what its colours mean (or why there are none yet). */
@Component({
  selector: 'fpv-route-colours',
  imports: [SegmentedComponent],
  template: `
    <rf-segmented label="Colour the route by" [options]="metrics" [value]="ws.weatherMetric()" (valueChange)="ws.weatherMetric.set($event)" />
    @if (!coloured()) {
      <div class="keys" role="status">{{ note() }}</div>
    } @else if (keys()) {
      <div class="keys" aria-label="Route colours">
        @for (k of legend(); track k.label) {
          <span><i [style.background]="k.color"></i>{{ k.label }}</span>
        }
      </div>
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 6px; }
    :host ::ng-deep .rf-segmented > button { height: 28px; font-size: 12px; padding: 0 10px; }
    .keys { display: flex; flex-wrap: wrap; gap: 4px 10px; font-size: 11px; color: var(--rf-text-2); max-width: 320px; }
    .keys span { display: inline-flex; align-items: center; gap: 4px; }
    .keys i { width: 14px; height: 5px; border-radius: 2px; }
  `,
})
export class RouteColoursComponent {
  protected readonly ws = inject(FpvWorkspace);
  private readonly wx = inject(RouteWeatherService);
  /** Off where the map carries the colour keys itself (phones) */
  readonly keys = input(true);

  protected readonly metrics = WeatherScale.METRICS.map((value) => ({ value, label: WeatherScale.title(value) }));
  protected readonly legend = computed(() => WeatherScale.legend(this.ws.weatherMetric()));

  /** Forecasts on the route to colour it by. */
  protected readonly coloured = computed(() => {
    const route = this.wx.route();
    return route !== null && !route.empty;
  });

  /** Why the route is not coloured yet. */
  protected readonly note = computed(() => {
    const status = this.wx.status();
    if (status === 'error') return 'The forecast could not be loaded';
    return status === 'ready' ? 'No forecast covers this flight time' : 'Loading the forecast…';
  });
}
