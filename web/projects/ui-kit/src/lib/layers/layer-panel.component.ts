import { Component, computed, inject, input, output } from '@angular/core';
import { LAYER_GROUP_TITLE, LayerGroup } from './layer-definition';
import { LayerEntry, LayerRegistry } from './layer-registry';
import { HeatLegendComponent } from '../ui/ui-components';

/**
 * One collapsible section of the layer panel: a title, what it holds in a few words and, when
 * given, how many of its layers are on.
 */
@Component({
  selector: 'rf-layer-section',
  template: `
    <details [open]="open()">
      <summary>
        <span class="row">
          <span class="title">{{ title() }}</span>
          @if (badge()) {
            <span class="badge">{{ badge() }}</span>
          }
          <span class="chevron" aria-hidden="true">▾</span>
        </span>
        <span class="hint">{{ hint() }}</span>
      </summary>
      <div class="content"><ng-content /></div>
    </details>
  `,
  styles: `
    :host { display: block; border-top: 1px solid var(--rf-border); }
    summary { list-style: none; cursor: pointer; display: flex; flex-direction: column; gap: 2px; padding: 10px 0; }
    summary::-webkit-details-marker { display: none; }
    .row { display: flex; align-items: center; gap: 8px; }
    .title { flex: 1; font-weight: 600; font-size: 14px; color: var(--rf-text); }
    .badge { font-size: 11px; color: var(--rf-ok-text); }
    .chevron { color: var(--rf-text-2); transition: transform 0.15s; }
    details:not([open]) .chevron { transform: rotate(-90deg); }
    .hint { font-size: 12px; color: var(--rf-text-2); }
    .content { display: flex; flex-direction: column; gap: 8px; padding-bottom: 12px; }
    summary:focus-visible { outline: 2px solid var(--rf-ok); outline-offset: 2px; border-radius: 4px; }
  `,
})
export class LayerSectionComponent {
  readonly title = input.required<string>();
  readonly hint = input('');
  readonly badge = input<string | null>(null);
  readonly open = input(true);
}

/** What the panel shows in one of its sections. */
interface PanelSection {
  items: LayerEntry[];
  on: number;
}

/**
 * Layer panel (MapSources.dc.html), in sections: Map (base pick-one and terrain), Coverage
 * (results, plus the host's coverage options projected with `rfCoverageOptions`), Weather (with the
 * forecast time slider) and Mission. Other projected content (view options) comes last.
 */
@Component({
  selector: 'rf-layer-panel',
  imports: [HeatLegendComponent, LayerSectionComponent],
  template: `
    <div class="head">
      <b>Layers</b>
      <button type="button" class="rf-icon-btn" aria-label="Close layers" (click)="closed.emit()">✕</button>
    </div>
    @if (bases().length || terrain().items.length) {
      <rf-layer-section title="Map" hint="Base map and terrain shading">
        @if (bases().length) {
          <div class="sub">{{ groupTitle('base') }}</div>
          <div class="base" role="radiogroup" aria-label="Base map">
            @for (e of bases(); track e.def.id) {
              <button type="button" role="radio" [attr.aria-checked]="e.visible" [class.on]="e.visible" (click)="registry.setVisible(e.def.id, true)">
                {{ e.def.title }} @if (e.visible) {<span aria-hidden="true">✓</span>}
              </button>
            }
          </div>
        }
        @if (terrain().items.length) {
          <div class="sub">{{ groupTitle('terrain') }}</div>
          @for (e of terrain().items; track e.def.id) {
            <label class="toggle"><input type="checkbox" [checked]="e.visible" (change)="toggle(e, $event)" /> {{ e.def.title }}</label>
          }
        }
      </rf-layer-section>
    }
    @if (coverage().items.length) {
      <rf-layer-section title="Coverage" hint="Signal over the area, its limits and how to read it" [badge]="badge(coverage())">
        @for (e of coverage().items; track e.def.id) {
          <label class="toggle"><input type="checkbox" [checked]="e.visible" (change)="toggle(e, $event)" /> {{ e.def.title }}</label>
          @if (e.def.id === 'results-coverage' && e.visible) {
            <rf-heat-legend />
          }
        }
        <ng-content select="[rfCoverageOptions]" />
      </rf-layer-section>
    }
    @if (weather().items.length) {
      <rf-layer-section title="Weather" hint="Forecast layers and the time they show" [badge]="badge(weather())" [open]="weather().on > 0">
        @for (e of weather().items; track e.def.id) {
          <label class="toggle"><input type="checkbox" [checked]="e.visible" (change)="toggle(e, $event)" /> {{ e.def.title }}</label>
        }
        <label class="rf-field">
          Forecast for · {{ timeLabel() }}
          <input type="range" min="0" [max]="forecastHours()" [value]="forecastOffset()" (input)="forecastOffsetChange.emit(+$any($event.target).value)" aria-label="Forecast time, hours from now" />
        </label>
      </rf-layer-section>
    }
    @if (mission().items.length) {
      <rf-layer-section title="Mission" hint="Extra drawings of the route" [badge]="badge(mission())">
        @for (e of mission().items; track e.def.id) {
          <label class="toggle"><input type="checkbox" [checked]="e.visible" (change)="toggle(e, $event)" /> {{ e.def.title }}</label>
        }
      </rf-layer-section>
    }
    <ng-content />
  `,
  styles: `
    :host { display: flex; flex-direction: column; width: 300px; max-width: calc(100vw - 32px); max-height: calc(100% - 32px); overflow-y: auto; padding: 14px 14px 2px; border-radius: 14px; background: rgba(16, 22, 29, 0.97); border: 1px solid var(--rf-border); }
    .head { display: flex; justify-content: space-between; align-items: center; font-size: 15px; padding-bottom: 10px; }
    .sub { font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; color: var(--rf-text-2); }
    .base { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
    .base button { height: 56px; border-radius: 10px; border: 1px solid var(--rf-border); background: var(--rf-surface-3); color: var(--rf-text-2); font-size: 13px; }
    .base button.on { border: 2px solid var(--rf-ok); color: var(--rf-text); }
    .toggle { display: flex; gap: 10px; align-items: center; min-height: 32px; }
    .toggle input { width: 18px; height: 18px; accent-color: var(--rf-ok); margin: 0; }
    input[type='range'] { accent-color: var(--rf-ok); width: 100%; }
  `,
})
export class LayerPanelComponent {
  protected readonly registry = inject(LayerRegistry);
  readonly forecastHours = input(48);
  readonly forecastOffset = input(0);
  /** ISO time the slider currently points to */
  readonly forecastTime = input<string | null>(null);
  readonly forecastOffsetChange = output<number>();
  readonly closed = output<void>();

  protected readonly bases = computed(() => this.section('base').items);
  protected readonly terrain = computed(() => this.section('terrain'));
  protected readonly coverage = computed(() => this.section('results'));
  protected readonly weather = computed(() => this.section('weather'));
  protected readonly mission = computed(() => this.section('mission'));

  protected groupTitle(g: LayerGroup): string {
    return LAYER_GROUP_TITLE[g];
  }

  protected badge(s: PanelSection): string | null {
    return s.on > 0 ? `${s.on} on` : null;
  }

  protected toggle(e: LayerEntry, event: Event): void {
    this.registry.setVisible(e.def.id, (event.target as HTMLInputElement).checked);
  }

  private section(group: LayerGroup): PanelSection {
    const items = this.registry.groups().find((g) => g.group === group)?.items ?? [];
    return { items, on: items.filter((e) => e.visible).length };
  }

  protected readonly timeLabel = computed(() => {
    const t = this.forecastTime();
    if (!t) return 'now';
    const d = new Date(t);
    const today = new Date();
    const sameDay = d.toDateString() === today.toDateString();
    const hm = d.toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
    return sameDay ? `today ${hm}` : `${d.toLocaleDateString('en-GB', { weekday: 'short', day: 'numeric', month: 'short' })} ${hm}`;
  });
}
