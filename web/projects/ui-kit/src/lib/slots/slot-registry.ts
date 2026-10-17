import { Injectable, Signal, Type, computed, signal } from '@angular/core';
import type { LayerDefinition } from '../layers/layer-definition';

export const SLOT_NAMES = [
  'header.actions',
  'side.panels',
  'map.layers',
  'map.overlays',
  'bottom.drawer',
  'verdict.reasons',
] as const;

export type SlotName = (typeof SLOT_NAMES)[number];

export function isSlotName(name: string): name is SlotName {
  return (SLOT_NAMES as readonly string[]).includes(name);
}

/** What a mount function receives besides its host element. */
export interface SlotMountContext {
  slot: SlotName;
  contributionId: string;
  /** Free-form data the slot host passes down (e.g. the selected leg for verdict.reasons). */
  data?: unknown;
}

export type SlotCleanup = () => void;

/**
 * One piece of UI placed into a named slot. Either an Angular component (app or ui-kit code)
 * or a framework-free mount function (runtime module bundles), or for `map.layers` a layer.
 */
export interface SlotContribution {
  id: string;
  slot: SlotName;
  /** Lower comes first; default 100. */
  order?: number;
  title?: string;
  /** Who contributed it: 'app', 'ui-kit' or a module id. */
  source?: string;
  component?: Type<unknown>;
  inputs?: Record<string, unknown>;
  mount?: (host: HTMLElement, ctx: SlotMountContext) => void | SlotCleanup;
  /** Only for slot `map.layers`. */
  layer?: LayerDefinition;
}

export class SlotRegistrationError extends Error {}

/** Registry of everything placed into the app's named slots. Signals drive the slot hosts. */
@Injectable({ providedIn: 'root' })
export class SlotRegistry {
  private readonly items = signal<readonly SlotContribution[]>([]);
  private readonly views = new Map<SlotName, Signal<SlotContribution[]>>();

  /** All contributions, for diagnostics. */
  readonly all: Signal<readonly SlotContribution[]> = this.items.asReadonly();

  /** Register a contribution; returns a function that removes it again. */
  register(contribution: SlotContribution): SlotCleanup {
    if (!isSlotName(contribution.slot)) {
      throw new SlotRegistrationError(`Unknown slot "${contribution.slot}". Known: ${SLOT_NAMES.join(', ')}`);
    }
    if (!contribution.id) throw new SlotRegistrationError('A slot contribution needs an id');
    const kinds = [contribution.component, contribution.mount, contribution.layer].filter((x) => x !== undefined);
    if (kinds.length !== 1) {
      throw new SlotRegistrationError(`Contribution "${contribution.id}" must have exactly one of component, mount or layer`);
    }
    if (contribution.layer && contribution.slot !== 'map.layers') {
      throw new SlotRegistrationError(`Layers go into slot "map.layers", not "${contribution.slot}"`);
    }
    if (this.items().some((c) => c.id === contribution.id && c.slot === contribution.slot)) {
      throw new SlotRegistrationError(`Contribution "${contribution.id}" is already in slot "${contribution.slot}"`);
    }
    const entry: SlotContribution = { order: 100, source: 'app', ...contribution };
    this.items.update((list) => [...list, entry]);
    return () => this.items.update((list) => list.filter((c) => c !== entry));
  }

  /** Remove everything a module contributed (module unloaded or failed). */
  unregisterSource(source: string): number {
    const before = this.items().length;
    this.items.update((list) => list.filter((c) => c.source !== source));
    return before - this.items().length;
  }

  /** Contributions of one slot, sorted by order then registration. */
  slot(name: SlotName): Signal<SlotContribution[]> {
    let view = this.views.get(name);
    if (!view) {
      view = computed(() =>
        this.items()
          .map((c, i) => ({ c, i }))
          .filter(({ c }) => c.slot === name)
          .sort((a, b) => (a.c.order ?? 100) - (b.c.order ?? 100) || a.i - b.i)
          .map(({ c }) => c),
      );
      this.views.set(name, view);
    }
    return view;
  }
}
