import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Map as MlMap } from 'maplibre-gl';
import { MapContext, MissionStore, provideMapView } from '@rfmap/ui-kit';
import { AltitudeView, FPV_ALTITUDE } from '../core/fpv-altitude';
import { FpvDataService } from '../core/fpv-data.service';
import { fpvAltitudeMock } from '../mock/fpv-altitude-mock';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { Route3dComponent } from './route-3d.component';

/** The parts of a MapLibre map the 3D mode touches, as spies. */
function fakeMap() {
  const sources = new Set<string>();
  const layers = new Set<string>();
  let terrain: unknown = null;
  let pitch = 0;
  let bearing = 0;
  const map = {
    getSource: vi.fn((id: string) => (sources.has(id) ? {} : undefined)),
    addSource: vi.fn((id: string) => sources.add(id)),
    setTerrain: vi.fn((t: unknown) => (terrain = t)),
    getTerrain: vi.fn(() => terrain),
    getLayer: vi.fn((id: string) => (layers.has(id) ? {} : undefined)),
    addLayer: vi.fn((l: { id: string }) => layers.add(l.id)),
    removeLayer: vi.fn((id: string) => layers.delete(id)),
    setMaxPitch: vi.fn(),
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

  it('raises the terrain and tilts the camera in 3D, and puts it back in 2D', () => {
    data().mapConfig.update((c) => ({ ...c!, dem: { pmtiles: '/tiles/terrain.pmtiles' } }));
    view.setMode('3d');
    fixture.detectChanges();
    expect(map.addSource).toHaveBeenCalledWith('fpv-terrain', expect.objectContaining({ type: 'raster-dem' }));
    expect(map.setTerrain).toHaveBeenLastCalledWith({ source: 'fpv-terrain', exaggeration: 2 });
    expect(map.addLayer).toHaveBeenCalledTimes(1);
    expect(map.setMaxPitch).toHaveBeenLastCalledWith(80);
    expect(map.dragRotate.enable).toHaveBeenCalled();
    expect(map.easeTo).toHaveBeenLastCalledWith({ pitch: 60, duration: 600 });
    expect(el().querySelector('.warn')).toBeNull();
    expect(el().textContent).toContain('Route heights…');

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

    view.profileOpen.set(false);
    [...el().querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent?.includes('Route heights'))!.click();
    expect(view.profileOpen()).toBe(true);

    map.rotate(45);
    el().querySelector<HTMLButtonElement>('button[aria-label="Back to 2D"]')!.click();
    expect(view.mode()).toBe('2d');
    fixture.detectChanges();
    expect(map.removeLayer).toHaveBeenCalledWith('fpv-route-3d');
    expect(map.setTerrain).toHaveBeenLastCalledWith(null);
    expect(map.dragRotate.disable).toHaveBeenCalled();
    expect(map.easeTo).toHaveBeenLastCalledWith({ pitch: 0, bearing: 0, duration: 400 });
  });

  it('draws the route flat and says so without terrain tiles', () => {
    data().mapConfig.update((c) => ({ ...c!, dem: undefined }));
    view.setMode('3d');
    fixture.detectChanges();
    expect(map.addSource).not.toHaveBeenCalled();
    expect(map.setTerrain).not.toHaveBeenCalled();
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
    expect(el().textContent).not.toContain('Route heights…');
    const layer = map.addLayer.mock.calls[0][0] as unknown as { scene: unknown };
    expect(layer.scene).not.toBeNull();
  });

  it('puts the map back flat at once when it is destroyed in 3D', () => {
    view.setMode('3d');
    fixture.detectChanges();
    fixture.destroy();
    expect(map.easeTo).toHaveBeenLastCalledWith({ pitch: 0, bearing: 0, duration: 0 });
  });
});
