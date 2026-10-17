import { Component, Pipe, PipeTransform, computed, input } from '@angular/core';
import { RfFormat, RfNumberKind } from './rf-format';

/**
 * Shared formatting pipe. `{{ value | rfNum:'dbm' }}` → "−98.74 dBm".
 * Use `| rfNum:kind:'full'` for the hover title.
 */
@Pipe({ name: 'rfNum', pure: true })
export class RfNumberPipe implements PipeTransform {
  transform(value: number | null | undefined, kind: RfNumberKind, mode: 'short' | 'full' | 'nounit' = 'short'): string {
    if (mode === 'full') return RfFormat.full(value, kind);
    return RfFormat.format(value, kind, { unit: mode !== 'nounit' });
  }
}

/** `<rf-num [value]="x" kind="db+" />` — rounded text, full value in the title, tabular mono figures. */
@Component({
  selector: 'rf-num',
  template: `{{ text() }}`,
  host: { class: 'rf-num', '[attr.title]': 'title()' },
})
export class RfNumComponent {
  readonly value = input<number | null | undefined>();
  readonly kind = input.required<RfNumberKind>();
  readonly unit = input(true);
  protected readonly text = computed(() => RfFormat.format(this.value(), this.kind(), { unit: this.unit() }));
  protected readonly title = computed(() => RfFormat.full(this.value(), this.kind()) || null);
}
