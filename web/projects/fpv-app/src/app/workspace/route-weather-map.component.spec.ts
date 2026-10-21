import { ComponentFixture, TestBed } from '@angular/core/testing';
import type { Map as MlMap } from 'maplibre-gl';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { LayerRegistry, MapContext, MissionStore, provideMapView } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';
import { RouteWeatherService } from '../core/route-weather.service';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { RouteWeatherMapComponent } from './route-weather-map.component';

interface Hover {
  title: string;
  lines: string[];
}

interface MapVm {
  active: () => boolean;
  hover: () => Hover | null;
  legend: () => { label: string }[];
  metric: { set: (m: string) => void };
}

/** Weather read-outs on the map, against a stand-in map that projects degrees to pixels. */
describe('RouteWeatherMapComponent', () => {
  let fixture: ComponentFixture<RouteWeatherMapComponent>;
  let wx: RouteWeatherService;
  let ctx: MapContext;
  const handlers = new Map<string, (e: unknown) => void>();
  const project = ([lon, lat]: [number, number]) => ({ x: lon * 10_000, y: -lat * 10_000 });
  const fakeMap = {
    project,
    on: (type: string, fn: (e: unknown) => void) => handlers.set(type, fn),
    off: vi.fn(),
  } as unknown as MlMap;

  const vm = () => fixture.componentInstance as unknown as MapVm;

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    TestBed.overrideComponent(RouteWeatherMapComponent, { set: { template: '', imports: [] } });
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    wx = TestBed.inject(RouteWeatherService);
    ctx = TestBed.inject(MapContext);
    ctx.map.set(fakeMap);
    fixture = TestBed.createComponent(RouteWeatherMapComponent);
    fixture.componentRef.setInput('compact', true);
    TestBed.inject(MissionStore).newMission(exampleMission());
    TestBed.tick();
    for (let i = 0; i < 100 && wx.status() !== 'ready'; i++) await MockApiHarness.wait(100);
    TestBed.tick();
  });

  afterEach(() => {
    handlers.clear();
    MockApiHarness.tearDown();
  });

  /** Screen position halfway along leg `leg`. */
  function midLeg(leg: number): { x: number; y: number } {
    const s = wx.timeline()!.stops;
    const a = project([s[leg].lon, s[leg].lat]);
    const b = project([s[leg + 1].lon, s[leg + 1].lat]);
    return { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
  }

  it('is active with forecasts on the route and the layer on', () => {
    expect(vm().active()).toBe(true);
    TestBed.inject(LayerRegistry).setVisible('results-route-weather', false);
    expect(vm().active()).toBe(false);
  });

  it('reads the weather where the cursor meets the route, and clears it off the route', () => {
    handlers.get('mousemove')!({ point: midLeg(2) });
    const h = vm().hover()!;
    expect(h.title).toMatch(/^Leg 3 · WP2 → B · \d+\.\d km from A · ETA \d\d:\d\d$/);
    expect(h.lines[0]).toMatch(/m\/s from [NESW]+ · (head|tail) /);
    expect(h.lines[1]).toMatch(/°C · rain /);
    handlers.get('mousemove')!({ point: { x: 0, y: 0 } });
    expect(vm().hover()).toBeNull();
    handlers.get('mousemove')!({ point: midLeg(0) });
    handlers.get('mouseout')!({});
    expect(vm().hover()).toBeNull();
  });

  it('pins the read-out on a tap near the route (phones)', () => {
    const s = wx.timeline()!.stops;
    ctx.emitClick({ lat: (s[1].lat + s[2].lat) / 2, lon: (s[1].lon + s[2].lon) / 2 });
    expect(vm().hover()?.title).toMatch(/^Leg 2 · WP1 → WP2/);
  });

  it('changes the legend with the metric', () => {
    vm().metric.set('rain');
    expect(vm().legend()[0].label).toBe('dry');
  });
});
