import { GeolocateControl, NavigationControl } from 'maplibre-gl';
import type { ControlPosition, IControl } from 'maplibre-gl';
import { describe, expect, it } from 'vitest';
import { MapControls } from './map-controls';

/** Records what a map view adds to its MapLibre map. */
class RecordingMap {
  readonly added: { control: IControl; position?: ControlPosition }[] = [];

  addControl(control: IControl, position?: ControlPosition): this {
    this.added.push({ control, position });
    return this;
  }
}

describe('MapControls', () => {
  it('adds the zoom buttons and the location button to the bottom-right corner', () => {
    const map = new RecordingMap();
    new MapControls({ zoom: true, locate: true }).addTo(map);
    expect(map.added.map((a) => a.position)).toEqual(['bottom-right', 'bottom-right']);
    expect(map.added[0].control).toBeInstanceOf(NavigationControl);
    // added last, so MapLibre stacks it above the zoom buttons
    expect(map.added[1].control).toBeInstanceOf(GeolocateControl);
  });

  it('leaves the zoom buttons out on phones but keeps the location button', () => {
    const controls = new MapControls({ zoom: false, locate: true });
    const created = controls.create();
    expect(created).toHaveLength(1);
    expect(created[0]).toBeInstanceOf(GeolocateControl);
    expect(controls.any).toBe(true);
  });

  it('jumps to the user once, without following them', () => {
    const [locate] = new MapControls({ zoom: false, locate: true }).create() as GeolocateControl[];
    expect(locate.options.trackUserLocation).toBe(false);
    expect(locate.options.fitBoundsOptions?.maxZoom).toBe(MapControls.locateMaxZoom);
  });

  it('reports an empty corner when no buttons are wanted', () => {
    const controls = new MapControls({ zoom: false, locate: false });
    expect(controls.create()).toEqual([]);
    expect(controls.any).toBe(false);
  });
});
