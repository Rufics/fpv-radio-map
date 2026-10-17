import { NgComponentOutlet } from '@angular/common';
import {
  Component,
  Directive,
  ElementRef,
  OnDestroy,
  OnInit,
  computed,
  inject,
  input,
  signal,
} from '@angular/core';
import { SlotCleanup, SlotContribution, SlotName, SlotRegistry } from './slot-registry';

/** Runs a framework-free mount function on its element and its cleanup on destroy. */
@Directive({ selector: '[rfMount]' })
export class MountDirective implements OnInit, OnDestroy {
  readonly rfMount = input.required<SlotContribution>();
  readonly rfMountData = input<unknown>();
  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef);
  private cleanup: SlotCleanup | undefined;
  readonly failed = signal<string | null>(null);

  ngOnInit(): void {
    const c = this.rfMount();
    try {
      const result = c.mount?.(this.host.nativeElement, { slot: c.slot, contributionId: c.id, data: this.rfMountData() });
      if (typeof result === 'function') this.cleanup = result;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      this.host.nativeElement.textContent = `Module UI “${c.source ?? c.id}” failed: ${msg}`;
      this.host.nativeElement.classList.add('rf-slot-failed');
      this.failed.set(msg);
    }
  }

  ngOnDestroy(): void {
    try {
      this.cleanup?.();
    } catch {
      /* a module's cleanup error must not break the app */
    }
  }
}

/**
 * Renders every contribution of a named slot.
 * `<rf-slot-host name="side.panels" />`
 */
@Component({
  selector: 'rf-slot-host',
  imports: [NgComponentOutlet, MountDirective],
  template: `
    @for (c of items(); track c.id) {
      @if (c.component) {
        <ng-container *ngComponentOutlet="c.component; inputs: c.inputs ?? {}" />
      } @else if (c.mount) {
        <div class="rf-slot-item" [attr.data-contribution]="c.id" [attr.data-source]="c.source" [rfMount]="c" [rfMountData]="data()"></div>
      }
    }
  `,
  host: { class: 'rf-slot-host', '[attr.data-slot]': 'name()', '[class.rf-slot-empty]': 'items().length === 0' },
})
export class SlotHostComponent {
  readonly name = input.required<SlotName>();
  readonly data = input<unknown>();
  private readonly registry = inject(SlotRegistry);
  protected readonly items = computed(() => this.registry.slot(this.name())().filter((c) => !c.layer));
}
