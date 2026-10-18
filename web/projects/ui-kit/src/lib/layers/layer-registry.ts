import { Injectable, Signal, computed, signal } from '@angular/core';
import type { Map as MlMap } from 'maplibre-gl';
import { LAYER_GROUP_RANK, LayerDefinition, LayerGroup } from './layer-definition';

export interface LayerEntry {
  def: LayerDefinition;
  visible: boolean;
}

/** Add, order, toggle and describe map layers. One instance per map view. */
@Injectable()
export class LayerRegistry {
  private readonly entries = signal<readonly LayerEntry[]>([]);
  /** Ids of the layers whose sources and style layers are on the map. */
  private readonly attached = new Set<string>();
  private map?: MlMap;
  private ready = false;

  readonly layers: Signal<readonly LayerEntry[]> = this.entries.asReadonly();

  /** Layers for the layer panel, grouped in draw order. */
  readonly groups = computed(() => {
    const groups = new Map<LayerGroup, LayerEntry[]>();
    for (const e of this.sorted(this.entries())) {
      if (e.def.internal) continue;
      const list = groups.get(e.def.group) ?? [];
      list.push(e);
      groups.set(e.def.group, list);
    }
    return [...groups.entries()].map(([group, items]) => ({ group, items }));
  });

  /** Credits of the visible layers, de-duplicated, for the attribution line. */
  readonly attributions = computed(() => {
    const seen = new Set<string>();
    for (const e of this.sorted(this.entries())) {
      if (e.visible && e.def.attribution) seen.add(e.def.attribution);
    }
    return [...seen];
  });

  register(def: LayerDefinition): () => void {
    if (this.entries().some((e) => e.def.id === def.id)) {
      throw new Error(`Layer "${def.id}" is already registered`);
    }
    const visible = def.defaultVisible ?? true;
    let entry: LayerEntry = { def, visible };
    if (def.exclusive && visible && this.entries().some((e) => e.def.group === def.group && e.def.exclusive && e.visible)) {
      entry = { def, visible: false };
    }
    this.entries.update((list) => [...list, entry]);
    if (this.map && this.ready && this.wanted(entry)) this.attachOne(entry);
    return () => this.unregister(def.id);
  }

  unregister(id: string): void {
    const entry = this.entries().find((e) => e.def.id === id);
    if (!entry) return;
    if (this.map && this.ready && this.attached.has(id)) entry.def.detach(this.map);
    this.attached.delete(id);
    this.entries.update((list) => list.filter((e) => e !== entry));
  }

  isVisible(id: string): boolean {
    return this.entries().find((e) => e.def.id === id)?.visible ?? false;
  }

  get(id: string): LayerDefinition | undefined {
    return this.entries().find((e) => e.def.id === id)?.def;
  }

  setVisible(id: string, visible: boolean): void {
    const target = this.entries().find((e) => e.def.id === id);
    if (!target) return;
    this.entries.update((list) =>
      list.map((e) => {
        if (e.def.id === id) return { ...e, visible };
        if (visible && target.def.exclusive && e.def.exclusive && e.def.group === target.def.group) {
          return { ...e, visible: false };
        }
        return e;
      }),
    );
    if (this.map && this.ready) {
      for (const e of this.sorted(this.entries())) {
        if (this.attached.has(e.def.id)) e.def.setVisible(this.map, e.visible);
        else if (this.wanted(e)) this.attachOne(e);
      }
    }
  }

  toggle(id: string): void {
    this.setVisible(id, !this.isVisible(id));
  }

  /** Called by the map view once the style is loaded. */
  connect(map: MlMap): void {
    this.map = map;
    this.ready = true;
    for (const e of this.sorted(this.entries())) {
      if (this.wanted(e)) this.attachOne(e);
    }
  }

  disconnect(): void {
    this.map = undefined;
    this.ready = false;
    this.attached.clear();
  }

  /** Whether a layer belongs on the map now: always, or once shown for lazy layers. */
  private wanted(entry: LayerEntry): boolean {
    return !entry.def.lazy || entry.visible;
  }

  private attachOne(entry: LayerEntry): void {
    const map = this.map;
    if (!map) return;
    const beforeId = this.firstStyleLayerAbove(entry.def, map);
    try {
      entry.def.attach(map, beforeId);
      entry.def.setVisible(map, entry.visible);
      this.attached.add(entry.def.id);
    } catch (e) {
      console.error(`Layer "${entry.def.id}" could not be added`, e);
    }
  }

  private firstStyleLayerAbove(def: LayerDefinition, map: MlMap): string | undefined {
    const rank = this.rank(def);
    const above = this.sorted(this.entries()).filter((e) => this.rank(e.def) > rank);
    for (const e of above) {
      const id = e.def.styleLayerIds().find((lid) => map.getLayer(lid));
      if (id) return id;
    }
    return undefined;
  }

  private rank(def: LayerDefinition): number {
    return LAYER_GROUP_RANK[def.group] + (def.order ?? 0) / 1000;
  }

  private sorted(list: readonly LayerEntry[]): LayerEntry[] {
    return [...list].sort((a, b) => this.rank(a.def) - this.rank(b.def));
  }
}
