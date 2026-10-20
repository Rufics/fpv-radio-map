import { GeolocateControl, NavigationControl } from 'maplibre-gl';
import type { ControlPosition, IControl } from 'maplibre-gl';

/** Which of the corner buttons a map view shows. */
export interface MapControlsOptions {
  /** Zoom in/out buttons (phones zoom with two fingers instead). */
  zoom: boolean;
  /** "Find my location": asks the browser for the position once and moves the map there. */
  locate: boolean;
}

/** The part of a MapLibre map the controls are added to and removed from. */
export interface ControlHost {
  addControl(control: IControl, position?: ControlPosition): unknown;
  removeControl(control: IControl): unknown;
}

/**
 * The buttons in the bottom-right corner of one map view. They stack upwards from the corner, so
 * the location button sits above the zoom buttons. Bottom-left overlays (attribution, hints) keep
 * `--rf-map-controls-reserve` free on the right to stay clear of this column.
 */
export class MapControls {
  static readonly position: ControlPosition = 'bottom-right';
  /** How close "Find my location" zooms in, so the pilot still sees the area around them. */
  static readonly locateMaxZoom = 14;

  private installed: IControl[] = [];
  private shown: MapControlsOptions = { zoom: false, locate: false };

  /** True when the options put at least one button in the corner. */
  static any(options: MapControlsOptions): boolean {
    return options.zoom || options.locate;
  }

  /** The controls in the order they are added; MapLibre stacks later bottom controls above. */
  static create(options: MapControlsOptions): IControl[] {
    const controls: IControl[] = [];
    if (options.zoom) controls.push(new NavigationControl({ showCompass: false }));
    if (options.locate) controls.push(MapControls.createLocate());
    return controls;
  }

  /**
   * Show exactly the buttons the options ask for. Changed options replace the whole column, so
   * the location button stays above the zoom buttons whichever was turned on last.
   */
  apply(map: ControlHost, options: MapControlsOptions): void {
    if (MapControls.same(options, this.shown)) return;
    for (const control of this.installed) map.removeControl(control);
    this.installed = MapControls.create(options);
    for (const control of this.installed) map.addControl(control, MapControls.position);
    this.shown = { ...options };
  }

  private static same(a: MapControlsOptions, b: MapControlsOptions): boolean {
    return a.zoom === b.zoom && a.locate === b.locate;
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
