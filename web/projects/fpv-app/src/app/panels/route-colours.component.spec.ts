import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MissionStore, RfmapApiClient, provideMapView } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { RouteWeatherService } from '../core/route-weather.service';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { RouteColoursComponent } from './route-colours.component';

/** The route colour picker and legend of weather mode, rendered, with the dev mock behind it. */
describe('RouteColoursComponent', () => {
  let wx: RouteWeatherService;
  let ws: FpvWorkspace;

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    wx = TestBed.inject(RouteWeatherService);
    ws = TestBed.inject(FpvWorkspace);
    ws.weatherMode.set(true);
  });

  afterEach(() => MockApiHarness.tearDown());

  function render(): HTMLElement {
    const fixture = TestBed.createComponent(RouteColoursComponent);
    fixture.detectChanges();
    return fixture.nativeElement as HTMLElement;
  }

  async function settle(done: () => boolean): Promise<void> {
    TestBed.tick();
    for (let i = 0; i < 100 && !done(); i++) await MockApiHarness.wait(100);
    TestBed.tick();
  }

  it('says the forecast is loading, then shows the legend of the chosen value', async () => {
    TestBed.inject(MissionStore).newMission(exampleMission());
    TestBed.tick();
    expect(render().textContent).toContain('Loading the forecast');
    await settle(() => wx.status() === 'ready');
    expect(render().textContent).toContain('tailwind');
    const rain = [...render().querySelectorAll('button')].find((b) => b.textContent?.trim() === 'Rain')!;
    rain.click();
    expect(ws.weatherMetric()).toBe('rain');
    expect(render().textContent).toContain('dry');
  });

  it('tells a forecast too short to colour the route from no forecast', async () => {
    let calls = 0;
    // only the first sample has a forecast (no return leg, which would share the pilot's cached forecast)
    vi.spyOn(TestBed.inject(RfmapApiClient), 'weatherPoint').mockImplementation(async () => (calls++ === 0 ? { tier: 'hourly', temperatureC: 4 } : { tier: 'none' }));
    TestBed.inject(MissionStore).newMission({ ...exampleMission(), returnToPilot: false });
    await settle(() => wx.status() === 'ready');
    expect(render().textContent).toContain('covers too little of the route');
  });

  it('leaves the legend out where the map carries it', async () => {
    TestBed.inject(MissionStore).newMission(exampleMission());
    await settle(() => wx.status() === 'ready');
    const fixture = TestBed.createComponent(RouteColoursComponent);
    fixture.componentRef.setInput('keys', false);
    fixture.detectChanges();
    const el = fixture.nativeElement as HTMLElement;
    expect(el.querySelector('[aria-label="Route colours"]')).toBeNull();
    expect(el.querySelectorAll('button')).toHaveLength(4);
  });

  it('says why the route is not coloured: failed or beyond the forecast', async () => {
    const store = TestBed.inject(MissionStore);
    const spy = vi.spyOn(TestBed.inject(RfmapApiClient), 'weatherPoint').mockRejectedValue(new Error('down'));
    store.newMission(exampleMission());
    await settle(() => wx.status() === 'error');
    expect(render().textContent).toContain('could not be loaded');
    spy.mockRestore();
    store.update((m) => ({ ...m, time: { mode: 'planned', utc: new Date(Date.now() + 40 * 86400e3).toISOString() } }));
    await settle(() => wx.status() === 'ready');
    expect(render().textContent).toContain('No forecast covers this flight time');
  });
});
