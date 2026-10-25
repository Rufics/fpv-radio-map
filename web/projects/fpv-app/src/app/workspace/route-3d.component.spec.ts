import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Map as MlMap } from 'maplibre-gl';
import { LayerRegistry, MapContext, MissionStore, provideMapView } from '@rfmap/ui-kit';
import { AltitudeView, FPV_ALTITUDE } from '../core/fpv-altitude';
import { FpvDataService } from '../core/fpv-data.service';
import { fpvAltitudeMock } from '../mock/fpv-altitude-mock';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { ChaseView } from '../core/fpv-chase';
import { Route3dComponent, SKY_3D } from './route-3d.component';

/** The parts of a MapLibre map the 3D mode touches, as spies. */
function fakeMap() {
  const sources = new Set<string>();
  const layers = new Set<string>();
  let terrain: unknown = null;
  let pitch = 0;
  let bearing = 0;
  const container = document.createElement('div');
  const map = {
    getSource: vi.fn((id: string) => (sources.has(id) ? {} : undefined)),
    addSource: vi.fn((id: string) => sources.add(id)),
    removeSource: vi.fn((id: string) => sources.delete(id)),
    setTerrain: vi.fn((t: unknown) => (terrain = t)),
    getTerrain: vi.fn(() => terrain),
    getLayer: vi.fn((id: string) => (layers.has(id) ? {} : undefined)),
    addLayer: vi.fn((l: { id: string }) => layers.add(l.id)),
    removeLayer: vi.fn((id: string) => layers.delete(id)),
    getLayersOrder: () => [...layers],
    setPaintProperty: vi.fn(),
    getContainer: () => container,
    cameraForBounds: vi.fn(() => ({ center: [35.2, 48.5], zoom: 12 })),
    setMaxPitch: vi.fn(),
    setSky: vi.fn(),
    calculateCameraOptionsFromTo: vi.fn(() => ({ center: [35.2, 48.5], zoom: 16, pitch: 62, bearing: 40 })),
    jumpTo: vi.fn(),
    dragRotate: { enable: vi.fn(), disable: vi.fn() },
    easeTo: vi.fn((o: { pitch?: number; bearing?: number }) => {
      pitch = o.pitch ?? pitch;
      bearing = o.bearing ?? bearing;
    }),
    getPitch: () => pitch,
    getBearing: () => bearing,
    triggerRepaint: vi.fn(),
    rotate: (deg: number) => (bearing = deg),
  };
  return map;
}

describe('Route3dComponent', () => {
  let fixture: ComponentFixture<Route3dComponent>;
  let view: AltitudeView;
  let map: ReturnType<typeof fakeMap>;
  const el = () => fixture.nativeElement as HTMLElement;
  const data = () => TestBed.inject(FpvDataService);

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    await MockApiHarness.settle(data().load());
    view = TestBed.inject(AltitudeView);
    map = fakeMap();
    TestBed.inject(MapContext).map.set(map as unknown as MlMap);
    fixture = TestBed.createComponent(Route3dComponent);
  });

  afterEach(() => MockApiHarness.tearDown());

  it('draws nothing in 2D', () => {
    fixture.detectChanges();
    expect(el().querySelector('.panel')).toBeNull();
    expect(map.addLayer).not.toHaveBeenCalled();
    expect(map.setMaxPitch).toHaveBeenCalledWith(60);
  });

  it('raises tinted, shaded terrain and tilts the camera in 3D, and puts it back in 2D', () => {
    const suppress = vi.spyOn(TestBed.inject(LayerRegistry), 'suppress');
    data().mapConfig.update((c) => ({ ...c!, dem: { pmtiles: '/tiles/terrain.pmtiles' } }));
    view.setMode('3d');
    fixture.detectChanges();
    expect(map.addSource).toHaveBeenCalledWith('fpv-terrain', expect.objectContaining({ type: 'raster-dem' }));
    expect(map.addSource).toHaveBeenCalledWith('fpv-relief', expect.objectContaining({ type: 'raster-dem' }));
    expect(map.setTerrain).toHaveBeenLastCalledWith({ source: 'fpv-terrain', exaggeration: 2 });
    const added = map.addLayer.mock.calls.map((c) => c[0].id);
    expect(added).toEqual(expect.arrayContaining(['fpv-relief-colour', 'fpv-relief-shade', 'fpv-route-3d']));
    // the flat 2D route, heatmap and terrain step aside, the 0 dB edge stays
    const hidden = suppress.mock.lastCall![0] as string[];
    expect(hidden).toEqual(expect.arrayContaining(['terrain-hillshade', 'terrain-contours']));
    expect(hidden).not.toContain('results-limits');
    expect(map.getContainer().classList.contains('fpv-3d')).toBe(true);
    expect(map.setMaxPitch).toHaveBeenLastCalledWith(80);
    expect(map.dragRotate.enable).toHaveBeenCalled();
    expect(map.easeTo).toHaveBeenLastCalledWith({ pitch: 60, duration: 600 });
    expect(el().querySelector('.warn')).toBeNull();

    const [ex, tilt] = [...el().querySelectorAll<HTMLInputElement>('input[type=range]')];
    ex.value = '1';
    ex.dispatchEvent(new Event('input'));
    fixture.detectChanges();
    expect(map.setTerrain).toHaveBeenLastCalledWith({ source: 'fpv-terrain', exaggeration: 1 });
    expect(el().querySelector('.tag')?.textContent).toContain('true scale');
    tilt.value = '30';
    tilt.dispatchEvent(new Event('input'));
    fixture.detectChanges();
    expect(map.easeTo).toHaveBeenLastCalledWith({ pitch: 30, duration: 200 });
    expect(el().textContent).toContain('Tilt 30°');

    // the corridor is off by default and the checkbox turns it on
    const corridor = el().querySelector<HTMLInputElement>('.check input')!;
    expect(corridor.checked).toBe(false);
    corridor.checked = true;
    corridor.dispatchEvent(new Event('change'));
    expect(view.corridor3d()).toBe(true);
    expect([...el().querySelectorAll('select option')].map((o) => o.textContent)).toEqual(['0 dB edge']);

    map.rotate(45);
    el().querySelector<HTMLButtonElement>('button[aria-label="Back to 2D"]')!.click();
    expect(view.mode()).toBe('2d');
    fixture.detectChanges();
    expect(map.removeLayer).toHaveBeenCalledWith('fpv-route-3d');
    expect(map.removeLayer).toHaveBeenCalledWith('fpv-relief-colour');
    expect(map.removeSource).toHaveBeenCalledWith('fpv-relief');
    expect(map.setTerrain).toHaveBeenLastCalledWith(null);
    expect(suppress).toHaveBeenLastCalledWith([]);
    expect(map.getContainer().classList.contains('fpv-3d')).toBe(false);
    expect(map.dragRotate.disable).toHaveBeenCalled();
    expect(map.easeTo).toHaveBeenLastCalledWith({ pitch: 0, bearing: 0, duration: 400 });
  });

  it('offers to calculate the heights when the route has none', () => {
    const enable = vi.spyOn(view, 'enable').mockResolvedValue();
    view.setMode('3d');
    fixture.detectChanges();
    expect(el().querySelector('.empty')?.textContent).toContain('The route needs its heights');
    [...el().querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent?.includes('Calculate heights'))!.click();
    expect(enable).toHaveBeenCalled();
  });

  it('draws the route flat and says so without terrain tiles', () => {
    data().mapConfig.update((c) => ({ ...c!, dem: undefined }));
    view.setMode('3d');
    fixture.detectChanges();
    expect(map.addSource).not.toHaveBeenCalled();
    expect(map.setTerrain).not.toHaveBeenCalled();
    expect(map.addLayer.mock.calls.map((c) => c[0].id)).toEqual(['fpv-route-3d']);
    expect(el().querySelector('.warn')?.textContent).toContain('No terrain tiles');
  });

  it('hands the layer a scene once the corridor is calculated', () => {
    const store = TestBed.inject(MissionStore);
    const mission = { ...exampleMission(), modules: { [FPV_ALTITUDE]: {} } };
    store.newMission(mission);
    store.result.set({
      ...routeResult(mission, 'normal'),
      modules: { [FPV_ALTITUDE]: fpvAltitudeMock(mission) },
    });
    view.setMode('3d');
    fixture.detectChanges();
    expect(el().querySelector('.empty')).toBeNull();
    const layer = map.addLayer.mock.calls.find((c) => c[0].id === 'fpv-route-3d')![0] as unknown as { scene: unknown };
    expect(layer.scene).not.toBeNull();
    // framed from the side of the route
    expect(map.cameraForBounds).toHaveBeenCalled();
    expect(map.easeTo).toHaveBeenLastCalledWith(expect.objectContaining({ pitch: 60, zoom: 12, duration: 800 }));
  });

  it('puts the map back flat at once when it is destroyed in 3D', () => {
    view.setMode('3d');
    fixture.detectChanges();
    fixture.destroy();
    expect(map.easeTo).toHaveBeenLastCalledWith({ pitch: 0, bearing: 0, duration: 0 });
  });

  it('follows the drone from behind while chasing, and frames the route again after', () => {
    const store = TestBed.inject(MissionStore);
    const mission = { ...exampleMission(), modules: { [FPV_ALTITUDE]: {} } };
    store.newMission(mission);
    store.result.set({ ...routeResult(mission, 'normal'), modules: { [FPV_ALTITUDE]: fpvAltitudeMock(mission) } });
    view.setMode('3d');
    fixture.detectChanges();
    expect(map.setSky).toHaveBeenLastCalledWith(SKY_3D);
    const chase = TestBed.inject(ChaseView);
    el().querySelector<HTMLButtonElement>('.chase')!.click();
    chase.playing.set(false);
    chase.seek(5000);
    fixture.detectChanges();
    expect(el().querySelector('fpv-chase-hud')).not.toBeNull();
    const cam = chase.track()!.camera(5000, 2);
    expect(map.calculateCameraOptionsFromTo).toHaveBeenLastCalledWith(
      expect.objectContaining({ lng: cam.from.lon, lat: cam.from.lat }),
      cam.fromAltM,
      expect.objectContaining({ lng: cam.to.lon, lat: cam.to.lat }),
      cam.toAltM,
    );
    expect(map.jumpTo).toHaveBeenLastCalledWith(expect.objectContaining({ pitch: 62, bearing: 40 }));
    chase.stop();
    fixture.detectChanges();
    expect(el().querySelector('fpv-chase-hud')).toBeNull();
    expect(map.easeTo).toHaveBeenLastCalledWith(expect.objectContaining({ pitch: 60, duration: 800 }));
    // back to 2D: the sky goes too
    view.setMode('2d');
    fixture.detectChanges();
    expect(map.setSky).toHaveBeenLastCalledWith({});
  });
});
