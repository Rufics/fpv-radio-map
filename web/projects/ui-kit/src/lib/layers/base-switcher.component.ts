import { Component, computed, inject } from '@angular/core';
import { LayerRegistry } from './layer-registry';

/**
 * Map / Satellite switch on the map itself, as in other map apps: the pick-one base layers of the
 * layer registry as a segmented control. Shows nothing with fewer than two (no satellite built).
 */
@Component({
  selector: 'rf-base-switcher',
  template: `
    @if (bases().length > 1) {
      <div class="rf-segmented" role="group" aria-label="Map view">
        @for (e of bases(); track e.def.id) {
          <button
            type="button"
            [attr.aria-pressed]="e.visible"
            (click)="registry.setVisible(e.def.id, true)"
          >
            {{ e.def.title }}
          </button>
        }
      </div>
    }
  `,
  styles: `
    :host {
      display: block;
    }
    .rf-segmented {
      background: rgba(16, 22, 29, 0.95);
    }
    /* 28 + 2 × 3 padding + 2 × 1 border = the 36 px of the Layers button next to it */
    .rf-segmented > button {
      height: 28px;
      padding: 0 12px;
      font-size: 13px;
    }
  `,
})
export class BaseSwitcherComponent {
  protected readonly registry = inject(LayerRegistry);
  protected readonly bases = computed(() =>
    this.registry
      .layers()
      .filter((e) => e.def.group === 'base' && e.def.exclusive && !e.def.internal)
      .sort((a, b) => (a.def.order ?? 0) - (b.def.order ?? 0)),
  );
}
