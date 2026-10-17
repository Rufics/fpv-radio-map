import type { RouteResult } from '../models/api.models';
import type { Mission } from '../models/mission.models';
import type { RfFormat } from '../format/rf-format';
import type { SlotCleanup, SlotMountContext, SlotName } from '../slots/slot-registry';

/** Version of the bundle contract; a bundle declares the major it was built for in `kit`. */
export const UI_KIT_VERSION = '1.0.0';

/** Read-only view of the mission state for module bundles. */
export interface RfModuleStateView {
  mission(): Mission | null;
  result(): RouteResult | null;
  /** Called now and on every change; returns an unsubscribe function. */
  subscribe(listener: (state: { mission: Mission | null; result: RouteResult | null }) => void): () => void;
}

/** What a mounted contribution from a bundle may declare. */
export interface RfBundleContribution {
  id: string;
  slot: SlotName;
  order?: number;
  title?: string;
  mount: (host: HTMLElement, ctx: SlotMountContext) => void | SlotCleanup;
}

/** The object a bundle's `register` receives. */
export interface RfModuleUiApi {
  readonly kitVersion: string;
  readonly moduleId: string;
  readonly moduleVersion: string;
  readonly slots: { register(contribution: RfBundleContribution): SlotCleanup };
  readonly state: RfModuleStateView;
  readonly format: Pick<typeof RfFormat, 'format' | 'full'>;
}

/**
 * Shape of a module UI bundle (an ES module at ModuleInfo.ui.bundle):
 *
 * ```js
 * export const kit = '1';
 * export function register(api) {
 *   api.slots.register({ id: 'my-panel', slot: 'side.panels', mount(host) { host.textContent = 'Hi'; } });
 * }
 * ```
 */
export interface RfModuleUiBundle {
  /** Major version of the ui-kit contract the bundle targets, e.g. "1". */
  kit?: string;
  register?: (api: RfModuleUiApi) => void | Promise<void>;
  default?: { kit?: string; register?: (api: RfModuleUiApi) => void | Promise<void> };
}
