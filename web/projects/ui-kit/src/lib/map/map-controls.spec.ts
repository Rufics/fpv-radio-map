import { GeolocateControl, NavigationControl } from 'maplibre-gl';
import type { ControlPosition, IControl } from 'maplibre-gl';
import { describe, expect, it } from 'vitest';
import { MapControls } from './map-controls';

/** Keeps the controls a map view puts in its corner, in the order MapLibre would add them. */
class RecordingMap {
  readonly added: { control: IControl; position?: ControlPosition }[] = [];
  removed = 0;

  addControl(control: IControl, position?: ControlPosition): this {
    this.added.push({ control, position });
    return this;
  }

  removeControl(control: IControl): this {
    const i = this.added.findIndex((a) => a.control === control);
    if (i >= 0) this.added.splice(i, 1);
    this.removed++;
    return this;
  }

  kinds(): string[] {
    return this.added.map((a) => (a.control instanceof GeolocateControl ? 'locate' : 'zoom'));
  }
}

describe('MapControls', () => {
  it('adds the zoom buttons and the location button to the bottom-right corner', () => {
    const map = new RecordingMap();
    new MapControls().apply(map, { zoom: true, locate: true });
    expect(map.added.map((a) => a.position)).toEqual(['bottom-right', 'bottom-right']);
    expect(map.added[0].control).toBeInstanceOf(NavigationControl);
    // added last, so MapLibre stacks it above the zoom buttons
    expect(map.added[1].control).toBeInstanceOf(GeolocateControl);
  });

  it('leaves the zoom buttons out on phones but keeps the location button', () => {
    const created = MapControls.create({ zoom: false, locate: true });
    expect(created).toHaveLength(1);
    expect(created[0]).toBeInstanceOf(GeolocateControl);
    expect(MapControls.any({ zoom: false, locate: true })).toBe(true);
  });

  it('jumps to the user once, without following them', () => {
    const [locate] = MapControls.create({ zoom: false, locate: true }) as GeolocateControl[];
    expect(locate.options.trackUserLocation).toBe(false);
    expect(locate.options.fitBoundsOptions?.maxZoom).toBe(MapControls.locateMaxZoom);
  });

  it('follows changed options on a live map and keeps the location button on top', () => {
    const map = new RecordingMap();
    const controls = new MapControls();
    controls.apply(map, { zoom: false, locate: true });
    controls.apply(map, { zoom: true, locate: true });
    expect(map.kinds()).toEqual(['zoom', 'locate']);
    controls.apply(map, { zoom: false, locate: true });
    expect(map.kinds()).toEqual(['locate']);
    controls.apply(map, { zoom: false, locate: false });
    expect(map.kinds()).toEqual([]);
  });

  it('leaves the corner alone when the options did not change', () => {
    const map = new RecordingMap();
    const controls = new MapControls();
    controls.apply(map, { zoom: true, locate: true });
    const first = map.added.map((a) => a.control);
    controls.apply(map, { zoom: true, locate: true });
    expect(map.removed).toBe(0);
    expect(map.added.map((a) => a.control)).toEqual(first);
  });

  it('reports an empty corner when no buttons are wanted', () => {
    const map = new RecordingMap();
    new MapControls().apply(map, { zoom: false, locate: false });
    expect(map.added).toEqual([]);
    expect(MapControls.any({ zoom: false, locate: false })).toBe(false);
  });
});
