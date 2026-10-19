import { Injectable, InjectionToken, Injector, effect, inject, signal, untracked } from '@angular/core';
import { ModuleInfo } from '../models/api.models';
import { RfFormat } from '../format/rf-format';
import { SlotRegistry } from '../slots/slot-registry';
import { MissionStore } from '../state/mission-store';
import { RfModuleUiApi, RfModuleUiBundle, UI_KIT_VERSION } from './module-api';

/** How a bundle URL becomes a module. Replaced in tests. */
export type ModuleImporter = (url: string) => Promise<RfModuleUiBundle>;

export const MODULE_IMPORTER = new InjectionToken<ModuleImporter>('MODULE_IMPORTER', {
  factory: () => (url: string) => import(/* @vite-ignore */ /* webpackIgnore: true */ url) as Promise<RfModuleUiBundle>,
});

/** Origins bundles may be loaded from besides the page's own. */
export const MODULE_BUNDLE_ORIGINS = new InjectionToken<string[]>('MODULE_BUNDLE_ORIGINS', { factory: () => [] });

export type ModuleUiStatus =
  | { moduleId: string; state: 'loading' }
  | { moduleId: string; state: 'loaded'; contributions: number }
  | { moduleId: string; state: 'skipped'; reason: string }
  | { moduleId: string; state: 'failed'; error: string };

export interface LoadOptions {
  /** Alpha modules are admin-only. */
  includeAlpha?: boolean;
}

/**
 * Loads module UI bundles at runtime (dynamic import of ModuleInfo.ui.bundle) and lets them
 * register into the named slots they declared in ui.slots. A failing bundle is isolated:
 * its contributions are removed and the status says why.
 */
@Injectable({ providedIn: 'root' })
export class ModuleUiLoader {
  private readonly slots = inject(SlotRegistry);
  private readonly store = inject(MissionStore);
  private readonly importer = inject(MODULE_IMPORTER);
  private readonly origins = inject(MODULE_BUNDLE_ORIGINS);
  private readonly injector = inject(Injector);
  private readonly loaded = new Map<string, () => void>();

  readonly status = signal<ModuleUiStatus[]>([]);

  /** Loads one bundle after the other, so status and slot registration keep the list order. */
  loadAll(modules: ModuleInfo[], opts: LoadOptions = {}): Promise<ModuleUiStatus[]> {
    const withUi = modules.filter((m) => m.ui?.bundle && m.id);
    return withUi.reduce<Promise<ModuleUiStatus[]>>(
      (previous, m) => previous.then((results) => this.load(m, opts).then((s) => [...results, s])),
      Promise.resolve([]),
    );
  }

  async load(info: ModuleInfo, opts: LoadOptions = {}): Promise<ModuleUiStatus> {
    const id = info.id ?? '?';
    const skip = this.skipReason(info, opts);
    if (skip) return this.setStatus({ moduleId: id, state: 'skipped', reason: skip });
    if (this.loaded.has(id)) this.unload(id);
    this.setStatus({ moduleId: id, state: 'loading' });

    const allowed = new Set(info.ui?.slots ?? []);
    const cleanups: (() => void)[] = [];
    let count = 0;
    try {
      const url = this.resolveUrl(info.ui?.bundle ?? '');
      const mod = await this.importer(url);
      const register = mod.register ?? mod.default?.register;
      const kit = mod.kit ?? mod.default?.kit;
      if (typeof register !== 'function') throw new Error('bundle does not export register()');
      if (kit && kit.split('.')[0] !== UI_KIT_VERSION.split('.')[0]) {
        throw new Error(`bundle targets ui-kit ${kit}, this app has ${UI_KIT_VERSION}`);
      }
      const api: RfModuleUiApi = {
        kitVersion: UI_KIT_VERSION,
        moduleId: id,
        moduleVersion: info.version ?? '',
        slots: {
          register: (c) => {
            if (!allowed.has(c.slot)) throw new Error(`slot "${c.slot}" is not declared in ui.slots of ${id}`);
            const off = this.slots.register({ id: `${id}/${c.id}`, slot: c.slot, order: c.order, title: c.title, mount: c.mount, source: id });
            cleanups.push(off);
            count++;
            return off;
          },
        },
        state: this.stateView(cleanups),
        format: { format: RfFormat.format, full: RfFormat.full },
      };
      await register(api);
      this.loaded.set(id, () => cleanups.forEach((f) => f()));
      return this.setStatus({ moduleId: id, state: 'loaded', contributions: count });
    } catch (e) {
      cleanups.forEach((f) => f());
      this.slots.unregisterSource(id);
      return this.setStatus({ moduleId: id, state: 'failed', error: e instanceof Error ? e.message : String(e) });
    }
  }

  unload(moduleId: string): void {
    this.loaded.get(moduleId)?.();
    this.loaded.delete(moduleId);
    this.slots.unregisterSource(moduleId);
    this.status.update((s) => s.filter((x) => x.moduleId !== moduleId));
  }

  private skipReason(info: ModuleInfo, opts: LoadOptions): string | null {
    if (info.state && info.state !== 'active') return `module is ${info.state}`;
    if (info.lifecycle === 'alpha' && !opts.includeAlpha) return 'alpha modules are admin-only';
    return null;
  }

  private resolveUrl(bundle: string): string {
    if (!bundle) throw new Error('empty bundle URL');
    const base = typeof location !== 'undefined' ? location.href : 'http://localhost/';
    const url = new URL(bundle, base);
    const own = new URL(base).origin;
    if (url.origin !== own && !this.origins.includes(url.origin)) {
      throw new Error(`bundle origin ${url.origin} is not allowed`);
    }
    if (url.protocol !== 'https:' && url.protocol !== 'http:') throw new Error(`bundle URL scheme ${url.protocol} is not allowed`);
    return url.href;
  }

  private stateView(cleanups: (() => void)[]): RfModuleUiApi['state'] {
    const store = this.store;
    return {
      mission: () => untracked(store.mission),
      result: () => untracked(store.result),
      subscribe: (listener) => {
        const ref = effect(
          () => {
            const state = { mission: store.mission(), result: store.result() };
            untracked(() => {
              try {
                listener(state);
              } catch (e) {
                console.error('module state listener failed', e);
              }
            });
          },
          { injector: this.injector },
        );
        const off = () => ref.destroy();
        cleanups.push(off);
        return off;
      },
    };
  }

  private setStatus(s: ModuleUiStatus): ModuleUiStatus {
    this.status.update((list) => [...list.filter((x) => x.moduleId !== s.moduleId), s]);
    return s;
  }
}
