import { Injectable, signal } from '@angular/core';
import type { Map as MlMap, MapMouseEvent } from 'maplibre-gl';
import { LatLon } from '@rfmap/ui-kit';

/** [west, south, east, north] of the map on screen. */
export type ViewBounds = [number, number, number, number];

/**
 * Where the mouse is over the map and what part of the world the map shows, as signals for the
 * tools that follow them (the coverage mini-map, the line of sight). One per map stage.
 */
@Injectable()
export class MapPointer {
  /** The point under the mouse; null while the mouse is off the map. */
  readonly cursor = signal<LatLon | null>(null);
  readonly view = signal<ViewBounds | null>(null);
  private detach?: () => void;

  connect(map: MlMap): void {
    this.disconnect();
    const move = (e: MapMouseEvent) => this.cursor.set({ lat: e.lngLat.lat, lon: e.lngLat.lng });
    const out = () => this.cursor.set(null);
    const view = () => {
      const b = map.getBounds();
      this.view.set([b.getWest(), b.getSouth(), b.getEast(), b.getNorth()]);
    };
    map.on('mousemove', move);
    map.on('mouseout', out);
    map.on('move', view);
    view();
    this.detach = () => {
      map.off('mousemove', move);
      map.off('mouseout', out);
      map.off('move', view);
    };
  }

  disconnect(): void {
    this.detach?.();
    this.detach = undefined;
    this.cursor.set(null);
  }
}
