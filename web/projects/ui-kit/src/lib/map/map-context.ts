import { Injectable, Provider, signal } from '@angular/core';
import type { LngLatBoundsLike, Map as MlMap } from 'maplibre-gl';
import { LayerRegistry } from '../layers/layer-registry';
import { LatLon } from '../tools/geo';

/** The live MapLibre map of one map view, shared with tools, layer panel and module bundles. */
@Injectable()
export class MapContext {
  readonly map = signal<MlMap | null>(null);
  /** Last click on the map (picking). */
  readonly lastClick = signal<LatLon | null>(null);

  private readonly clickHandlers = new Set<(p: LatLon) => void>();

  onClick(handler: (p: LatLon) => void): () => void {
    this.clickHandlers.add(handler);
    return () => this.clickHandlers.delete(handler);
  }

  /** @internal called by the map view */
  emitClick(p: LatLon): void {
    this.lastClick.set(p);
    for (const h of this.clickHandlers) h(p);
  }

  fitTo(points: LatLon[], padding: number | { top: number; bottom: number; left: number; right: number } = 60): void {
    const map = this.map();
    if (!map || points.length === 0) return;
    const lats = points.map((p) => p.lat);
    const lons = points.map((p) => p.lon);
    const bounds: LngLatBoundsLike = [
      [Math.min(...lons), Math.min(...lats)],
      [Math.max(...lons), Math.max(...lats)],
    ];
    map.fitBounds(bounds, { padding, duration: 0, maxZoom: 14 });
  }
}

/** Providers for one map workspace: the layer registry and the map context. */
export function provideMapView(): Provider[] {
  return [LayerRegistry, MapContext];
}
