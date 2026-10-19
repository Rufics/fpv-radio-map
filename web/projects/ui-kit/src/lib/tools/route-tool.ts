import type { Feature, FeatureCollection } from 'geojson';
import { GeoJSONSource, LayerSpecification, Map as MlMap, Marker, SourceSpecification } from 'maplibre-gl';
import { StyleLayerSet } from '../layers/layer-definition';
import { LayerRegistry } from '../layers/layer-registry';
import { MapContext } from '../map/map-context';
import { LatLon, midpoint } from './geo';

export type LegDrawStatus = 'pending' | 'ok' | 'warn' | 'blocked' | 'missing' | 'return' | 'preview';

export interface DrawPoint extends LatLon {
  /** Text next to the marker, e.g. "Pilot · 544 + 2 m" */
  caption?: string;
  /** Accessible name of the marker button */
  ariaLabel?: string;
  /** Override the marker style and symbol (e.g. a turn-back point at the end of the route). */
  markerKind?: 'pilot' | 'waypoint' | 'target' | 'turnback';
  symbol?: string;
}

export interface DrawLeg {
  status: LegDrawStatus;
  /** Label at the middle of the leg, e.g. "3.9 km · V ✕" */
  label?: string;
}

export interface DrawMarker extends DrawPoint {
  kind: 'turnback' | 'obstruction' | 'relay';
  symbol: string;
}

export interface RouteDrawing {
  pilot: DrawPoint | null;
  waypoints: DrawPoint[];
  /** legs[i] connects point i to point i+1, with the pilot as point 0 */
  legs: DrawLeg[];
  /** Optional dashed return from a point to the pilot */
  /** `labelAt`: where the label sits, as a fraction from `from` to the pilot (default 0.5) */
  returnLeg?: { from: LatLon; label?: string; labelAt?: number } | null;
  markers?: DrawMarker[];
}

/** Route lines; markers are DOM buttons managed by RouteTool. */
class RouteLinesLayer extends StyleLayerSet {
  readonly id = 'mission-route';
  readonly title = 'Route';
  readonly group = 'mission' as const;
  override readonly internal = true;
  private data: FeatureCollection = { type: 'FeatureCollection', features: [] };

  protected sources(): Record<string, SourceSpecification> {
    return { 'rf-route': { type: 'geojson', data: this.data } };
  }

  protected layers(): LayerSpecification[] {
    const by = (s: string[]) => ['in', ['get', 'status'], ['literal', s]] as never;
    return [
      { id: 'route-casing', type: 'line', source: 'rf-route', layout: { 'line-cap': 'round', 'line-join': 'round' }, paint: { 'line-color': '#0B0F14', 'line-width': 8, 'line-opacity': 0.7 } },
      { id: 'route-solid', type: 'line', source: 'rf-route', filter: by(['ok', 'pending']), layout: { 'line-cap': 'round', 'line-join': 'round' }, paint: { 'line-color': ['match', ['get', 'status'], 'pending', '#7BE6D6', '#35D0BA'], 'line-width': 4 } },
      { id: 'route-blocked', type: 'line', source: 'rf-route', filter: by(['blocked']), layout: { 'line-cap': 'round' }, paint: { 'line-color': '#FF6B5B', 'line-width': 4, 'line-dasharray': [2.5, 1.75] } },
      { id: 'route-warn', type: 'line', source: 'rf-route', filter: by(['warn', 'return']), layout: { 'line-cap': 'round' }, paint: { 'line-color': '#F5A524', 'line-width': 3, 'line-dasharray': [2.7, 2] } },
      { id: 'route-missing', type: 'line', source: 'rf-route', filter: by(['missing']), layout: { 'line-cap': 'butt' }, paint: { 'line-color': '#A3AFBD', 'line-width': 3, 'line-dasharray': [1, 1.5] } },
      { id: 'route-preview', type: 'line', source: 'rf-route', filter: by(['preview']), layout: { 'line-cap': 'round' }, paint: { 'line-color': '#9DB4FF', 'line-width': 4, 'line-dasharray': [2, 1.5] } },
    ];
  }

  setData(fc: FeatureCollection, map?: MlMap): void {
    this.data = fc;
    this.pushData(map);
  }

  override attach(map: MlMap, beforeId: string | undefined): void {
    super.attach(map, beforeId);
    this.pushData(map);
  }

  private pushData(map: MlMap | undefined): void {
    (map?.getSource('rf-route') as GeoJSONSource | undefined)
      ?.setData(this.data)
      .catch((e: unknown) => console.error('Route lines could not be updated', e));
  }
}

export interface RouteToolOptions {
  /** Pilot is index -1, waypoints 0..n-1 */
  onMove?: (index: number, p: LatLon) => void;
  onSelectPoint?: (index: number) => void;
  onSelectLeg?: (legIndex: number) => void;
}

/**
 * Point and route tools: draws pilot A, waypoints, target B, legs and extra markers;
 * in edit mode points are draggable and a map click can add a point.
 */
export class RouteTool {
  private readonly lines = new RouteLinesLayer();
  private markers: Marker[] = [];
  private editable = false;
  private last: RouteDrawing | null = null;
  private readonly offRegistry: () => void;
  private addHandler: ((p: LatLon) => void) | null = null;
  private readonly offClick: () => void;

  constructor(
    private readonly registry: LayerRegistry,
    private readonly ctx: MapContext,
    private readonly options: RouteToolOptions = {},
  ) {
    this.offRegistry = registry.register(this.lines);
    this.offClick = ctx.onClick((p) => this.addHandler?.(p));
  }

  destroy(): void {
    this.clearMarkers();
    this.offRegistry();
    this.offClick();
  }

  setEditable(editable: boolean): void {
    this.editable = editable;
    if (this.last) this.draw(this.last);
  }

  /** While set, a click on the map calls the handler (place pilot / add point). */
  setAddPointHandler(handler: ((p: LatLon) => void) | null): void {
    this.addHandler = handler;
    const canvas = this.ctx.map()?.getCanvas();
    if (canvas) canvas.style.cursor = handler ? 'crosshair' : '';
  }

  draw(d: RouteDrawing): void {
    this.last = d;
    const map = this.ctx.map();
    const pts: LatLon[] = d.pilot ? [d.pilot, ...d.waypoints] : [...d.waypoints];
    const features: Feature[] = [];
    if (d.pilot) {
      d.legs.forEach((leg, i) => {
        const a = pts[i];
        const b = pts[i + 1];
        if (!a || !b) return;
        features.push({ type: 'Feature', properties: { status: leg.status, leg: i }, geometry: { type: 'LineString', coordinates: [[a.lon, a.lat], [b.lon, b.lat]] } });
      });
      if (d.returnLeg) {
        features.push({ type: 'Feature', properties: { status: 'return' }, geometry: { type: 'LineString', coordinates: [[d.returnLeg.from.lon, d.returnLeg.from.lat], [d.pilot.lon, d.pilot.lat]] } });
      }
    }
    this.lines.setData({ type: 'FeatureCollection', features }, map ?? undefined);
    if (!map) return;
    this.clearMarkers();

    if (d.pilot) {
      d.legs.forEach((leg, i) => {
        if (!leg.label || !pts[i] || !pts[i + 1]) return;
        this.addLabel(map, midpoint(pts[i], pts[i + 1]), leg.label, `rf-leg-label is-${leg.status}`, () => this.options.onSelectLeg?.(i));
      });
      if (d.returnLeg?.label) {
        const t = d.returnLeg.labelAt ?? 0.5;
        const at = { lat: d.returnLeg.from.lat + (d.pilot.lat - d.returnLeg.from.lat) * t, lon: d.returnLeg.from.lon + (d.pilot.lon - d.returnLeg.from.lon) * t };
        this.addLabel(map, at, d.returnLeg.label, 'rf-leg-label is-return');
      }
    }
    for (const m of d.markers ?? []) this.addPoint(map, m, `rf-marker rf-marker--${m.kind}`, m.symbol, -2, false);
    d.waypoints.forEach((w, i) => {
      const isTarget = i === d.waypoints.length - 1;
      const kind = w.markerKind ?? (isTarget ? 'target' : 'waypoint');
      this.addPoint(map, w, `rf-marker rf-marker--${kind}`, w.symbol ?? (isTarget ? 'B' : String(i + 1)), i, this.editable);
    });
    if (d.pilot) this.addPoint(map, d.pilot, 'rf-marker rf-marker--pilot', 'A', -1, this.editable);
  }

  private addPoint(map: MlMap, p: DrawPoint, cls: string, symbol: string, index: number, draggable: boolean): void {
    const el = document.createElement('div');
    el.className = 'rf-marker-wrap';
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = cls;
    btn.textContent = symbol;
    btn.setAttribute('aria-label', p.ariaLabel ?? p.caption ?? symbol);
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      if (index >= -1) this.options.onSelectPoint?.(index);
    });
    el.appendChild(btn);
    if (p.caption) {
      const cap = document.createElement('span');
      cap.className = 'rf-marker-caption';
      cap.textContent = p.caption;
      cap.setAttribute('aria-hidden', 'true');
      el.appendChild(cap);
    }
    const marker = new Marker({ element: el, draggable, anchor: 'center' }).setLngLat([p.lon, p.lat]).addTo(map);
    if (draggable) {
      marker.on('dragend', () => {
        const ll = marker.getLngLat();
        this.options.onMove?.(index, { lat: ll.lat, lon: ll.lng });
      });
    }
    this.markers.push(marker);
  }

  private addLabel(map: MlMap, p: LatLon, text: string, cls: string, onClick?: () => void): void {
    const el = document.createElement(onClick ? 'button' : 'div');
    el.className = cls;
    el.textContent = text;
    if (onClick) {
      (el as HTMLButtonElement).type = 'button';
      el.addEventListener('click', (e) => {
        e.stopPropagation();
        onClick();
      });
    }
    this.markers.push(new Marker({ element: el, anchor: 'center' }).setLngLat([p.lon, p.lat]).addTo(map));
  }

  private clearMarkers(): void {
    for (const m of this.markers) m.remove();
    this.markers = [];
  }
}
