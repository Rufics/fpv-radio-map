import { TestBed } from '@angular/core/testing';
import { Type } from '@angular/core';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MissionStore, RfmapApiClient, provideMapView } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { RouteWeatherService } from '../core/route-weather.service';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { WeatherBriefingComponent } from './weather-briefing.component';
import { WeatherCardComponent } from './weather-card.component';

/** The weather card and the briefing rendered with their templates, with the dev mock behind them. */
describe('weather card and briefing, rendered', () => {
  let wx: RouteWeatherService;

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    wx = TestBed.inject(RouteWeatherService);
    TestBed.inject(FpvWorkspace).weatherMode.set(true);
  });

  afterEach(() => MockApiHarness.tearDown());

  async function settle(done: () => boolean): Promise<void> {
    TestBed.tick();
    for (let i = 0; i < 100 && !done(); i++) await MockApiHarness.wait(100);
    TestBed.tick();
  }

  function render<T>(component: Type<T>): HTMLElement {
    const fixture = TestBed.createComponent(component);
    fixture.detectChanges();
    return fixture.nativeElement as HTMLElement;
  }

  it('card: a row per route point, the summary and the briefing button', async () => {
    TestBed.inject(MissionStore).newMission(exampleMission());
    await settle(() => wx.status() === 'ready');
    const el = render(WeatherCardComponent);
    expect(el.querySelectorAll('tbody tr')).toHaveLength(5);
    expect(el.querySelectorAll('.summary li')).toHaveLength(3);
    expect(el.textContent).toContain('At A,');
    expect(el.textContent).toContain('10-inch recon quad');
    const button = [...el.querySelectorAll('button')].find((b) => b.textContent?.includes('Weather briefing'))!;
    expect(button.disabled).toBe(false);
    button.click();
    expect(TestBed.inject(FpvWorkspace).briefingOpen()).toBe(true);
  });

  it('card: carries the route colours on phones only', async () => {
    TestBed.inject(MissionStore).newMission(exampleMission());
    await settle(() => wx.status() === 'ready');
    expect(render(WeatherCardComponent).querySelector('fpv-route-colours')).toBeNull();
    const fixture = TestBed.createComponent(WeatherCardComponent);
    fixture.componentRef.setInput('routeColours', true);
    fixture.detectChanges();
    expect((fixture.nativeElement as HTMLElement).querySelector('fpv-route-colours')).not.toBeNull();
  });

  it('card: loading, error with a retry, and beyond the forecast', async () => {
    const store = TestBed.inject(MissionStore);
    const spy = vi.spyOn(TestBed.inject(RfmapApiClient), 'weatherPoint').mockRejectedValue(new Error('forecast service down'));
    store.newMission({ ...exampleMission(), modules: {} });
    TestBed.tick();
    expect(render(WeatherCardComponent).textContent).toContain('Loading the forecast');
    await settle(() => wx.status() === 'error');
    const failed = render(WeatherCardComponent);
    expect(failed.textContent).toContain('forecast service down');
    expect(failed.textContent).toContain('standard atmosphere');
    spy.mockRestore();
    store.update((m) => ({ ...m, time: { mode: 'planned', utc: new Date(Date.now() + 40 * 86400e3).toISOString() } }));
    await settle(() => wx.status() === 'ready');
    expect(render(WeatherCardComponent).textContent).toContain('No forecast covers');
  });

  it('briefing: verdict, a card per point with its wind chart, and back to the map', async () => {
    TestBed.inject(MissionStore).newMission(exampleMission());
    await settle(() => wx.status() === 'ready');
    const ws = TestBed.inject(FpvWorkspace);
    ws.briefingOpen.set(true);
    const fixture = TestBed.createComponent(WeatherBriefingComponent);
    fixture.detectChanges();
    const el = fixture.nativeElement as HTMLElement;
    expect(el.querySelectorAll('article.pt')).toHaveLength(5);
    expect(el.querySelector('.verdict b')?.textContent).toBeTruthy();
    expect(el.textContent).toContain('Loading the hours around the ETA');
    for (let i = 0; i < 50 && el.querySelectorAll('figure').length < 5; i++) {
      await MockApiHarness.wait(100);
      fixture.detectChanges();
    }
    expect(el.querySelectorAll('figure polyline')).toHaveLength(5);
    expect(el.textContent).toContain('Source:');
    [...el.querySelectorAll('button')].find((b) => b.textContent?.includes('Back to map'))!.click();
    expect(ws.briefingOpen()).toBe(false);
  });
});
