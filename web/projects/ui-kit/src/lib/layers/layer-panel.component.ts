import { Component, computed, inject, input, output } from '@angular/core';
import { LAYER_GROUP_TITLE } from './layer-definition';
import { LayerRegistry } from './layer-registry';
import { HeatLegendComponent } from '../ui/ui-components';

/** Layer panel (MapSources.dc.html): base pick-one, toggles per group, forecast time slider. */
@Component({
  selector: 'rf-layer-panel',
  imports: [HeatLegendComponent],
  template: `
    <div class="head">
      <b>Layers</b>
      <button type="button" class="rf-icon-btn" aria-label="Close layers" (click)="closed.emit()">✕</button>
    </div>
    @for (g of registry.groups(); track g.group) {
      <div class="rf-eyebrow">{{ title(g.group) }}</div>
      @if (g.group === 'base') {
        <div class="base" role="radiogroup" aria-label="Base map">
          @for (e of g.items; track e.def.id) {
            <button type="button" role="radio" [attr.aria-checked]="e.visible" [class.on]="e.visible" (click)="registry.setVisible(e.def.id, true)">
              {{ e.def.title }} @if (e.visible) {<span aria-hidden="true">✓</span>}
            </button>
          }
        </div>
      } @else {
        @for (e of g.items; track e.def.id) {
          <label class="toggle">
            <input type="checkbox" [checked]="e.visible" (change)="registry.setVisible(e.def.id, $any($event.target).checked)" />
            {{ e.def.title }}
          </label>
          @if (e.def.id === 'results-coverage' && e.visible) {
            <rf-heat-legend />
          }
        }
        @if (g.group === 'weather') {
          <label class="rf-field">
            Forecast for · {{ timeLabel() }}
            <input type="range" min="0" [max]="forecastHours()" [value]="forecastOffset()" (input)="forecastOffsetChange.emit(+$any($event.target).value)" aria-label="Forecast time, hours from now" />
          </label>
        }
      }
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 10px; width: 300px; max-width: calc(100vw - 32px); max-height: calc(100% - 32px); overflow-y: auto; padding: 14px; border-radius: 14px; background: rgba(16, 22, 29, 0.97); border: 1px solid var(--rf-border); }
    .head { display: flex; justify-content: space-between; align-items: center; font-size: 15px; }
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

  protected title(g: keyof typeof LAYER_GROUP_TITLE): string {
    return LAYER_GROUP_TITLE[g];
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
