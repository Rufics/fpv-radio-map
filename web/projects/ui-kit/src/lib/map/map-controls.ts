import { GeolocateControl, NavigationControl } from 'maplibre-gl';
import type { ControlPosition, IControl } from 'maplibre-gl';

/** Which of the corner buttons a map view shows. */
export interface MapControlsOptions {
  /** Zoom in/out buttons (phones zoom with two fingers instead). */
  zoom: boolean;
  /** "Find my location": asks the browser for the position once and moves the map there. */
  locate: boolean;
}

/** The part of a MapLibre map the controls are added to. */
export interface ControlHost {
  addControl(control: IControl, position?: ControlPosition): unknown;
}

/**
 * The buttons in the bottom-right corner of a map view. They stack upwards from the corner, so
 * the location button sits above the zoom buttons. Bottom-left overlays (attribution, hints) keep
 * `--rf-map-controls-reserve` free on the right to stay clear of this column.
 */
export class MapControls {
  static readonly position: ControlPosition = 'bottom-right';
  /** How close "Find my location" zooms in, so the pilot still sees the area around them. */
  static readonly locateMaxZoom = 14;

  constructor(private readonly options: MapControlsOptions) {}

  /** True when the corner has at least one button, so overlays need to leave room for it. */
  get any(): boolean {
    return this.options.zoom || this.options.locate;
  }

  addTo(map: ControlHost): void {
    for (const control of this.create()) map.addControl(control, MapControls.position);
  }

  /** The controls in the order they are added; MapLibre stacks later bottom controls above. */
  create(): IControl[] {
    const controls: IControl[] = [];
    if (this.options.zoom) controls.push(new NavigationControl({ showCompass: false }));
    if (this.options.locate) controls.push(MapControls.createLocate());
    return controls;
  }

  private static createLocate(): GeolocateControl {
    return new GeolocateControl({
      positionOptions: { enableHighAccuracy: true, timeout: 15000 },
      fitBoundsOptions: { maxZoom: MapControls.locateMaxZoom },
      trackUserLocation: false,
      showUserLocation: true,
      showAccuracyCircle: true,
    });
  }
}
