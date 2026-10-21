import { ComponentFixture, TestBed } from '@angular/core/testing';
import { Type } from '@angular/core';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore, RouteTimeline, RouteWeather, provideMapView } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { HourlyWeather, MissionTimelineBuilder, RouteWeatherService } from '../core/route-weather.service';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { WeatherRibbon } from '../workspace/route-weather-map.component';
import { WeatherBriefingComponent, WindSparkline } from './weather-briefing.component';
import { WeatherCardComponent } from './weather-card.component';

/** View models of the weather card, the briefing and the map ribbon, with the dev mock behind them (templates not rendered). */
describe('weather views', () => {
  let store: MissionStore;
  let wx: RouteWeatherService;

  function vmOf<T>(component: Type<unknown>): T {
    const fixture: ComponentFixture<unknown> = TestBed.createComponent(component);
    return fixture.componentInstance as T;
  }

  async function loaded(): Promise<void> {
    TestBed.tick();
    for (let i = 0; i < 100 && wx.status() !== 'ready'; i++) await MockApiHarness.wait(100);
  }

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    for (const c of [WeatherCardComponent, WeatherBriefingComponent]) TestBed.overrideComponent(c, { set: { template: '', imports: [] } });
    store = TestBed.inject(MissionStore);
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    wx = TestBed.inject(RouteWeatherService);
  });

  afterEach(() => MockApiHarness.tearDown());

  it('card: launch weather, humidity and how arrival times are made', async () => {
    store.newMission(exampleMission());
    await loaded();
    const card = vmOf<{ launch: () => { name: string } | null; humidity: () => string; speedText: () => string; isReturn: () => boolean; short: (t: string) => string }>(WeatherCardComponent);
    expect(card.launch()?.name).toBe('A');
    expect(card.humidity()).toMatch(/^ · \d+% RH$/);
    expect(card.speedText()).toContain('10-inch recon quad');
    expect(card.isReturn()).toBe(true);
    expect(card.short('7.4 m/s')).toBe('7.4');
    store.update((m) => ({ ...m, drone: undefined }));
    expect(card.speedText()).toContain('assumed');
  });

  it('briefing: verdict, a card per route point with its rows, and the hours around the ETA', async () => {
    store.newMission(exampleMission());
    await loaded();
    const ws = TestBed.inject(FpvWorkspace);
    ws.briefingOpen.set(true);
    const b = vmOf<{ verdict: () => { title: string; tone: string }; cards: () => { key: { name: string }; rows: { label: string }[]; spark: unknown; hoursLoaded: boolean }[]; subtitle: () => string; close: () => void }>(WeatherBriefingComponent);
    expect(['ok', 'warn', 'blocked']).toContain(b.verdict().tone);
    expect(b.cards().map((c) => c.key.name)).toEqual(['A', 'WP1', 'WP2', 'B', 'A']);
    expect(b.cards()[0].rows.map((r) => r.label)).toContain('Air density');
    expect(b.subtitle()).toContain('5 route points');
    TestBed.tick();
    for (let i = 0; i < 50 && !b.cards().every((c) => c.hoursLoaded); i++) await MockApiHarness.wait(100);
    expect(b.cards().every((c) => c.spark !== null)).toBe(true);
    b.close();
    expect(ws.briefingOpen()).toBe(false);
  });

  it('sparkline: scales to the hours it has and marks the ETA', () => {
    const t0 = Date.UTC(2026, 9, 21, 10);
    const hours: HourlyWeather[] = [0, 1, 2].map((i) => ({ timeMs: t0 + i * 3600e3, weather: i === 1 ? null : { windSpeedMps: 4 + i } }));
    const s = new WindSparkline(hours);
    expect(s.points().split(' ')).toHaveLength(2);
    expect(s.xOf(t0 + 3600e3)).toBe(90);
    expect(s.xOf(t0 - 1)).toBeNull();
    expect(s.label()).toContain('peak 6.0 m/s');
    expect(new WindSparkline([]).label()).toBe('');
    expect(new WindSparkline([]).xOf(t0)).toBeNull();
  });

  it('ribbon: coloured pieces broken at every route point', async () => {
    const m = exampleMission();
    store.newMission(m);
    await loaded();
    const t = wx.timeline()!;
    const segs = new WeatherRibbon(t, wx.route()!).segments('headwind');
    expect(segs.length).toBeGreaterThanOrEqual(wx.route()!.readings().length - 1);
    expect(segs.every((s) => /^#[0-9A-F]{6}$/i.test(s.color))).toBe(true);
    // a piece never spans a waypoint: each lies on one leg
    const wp1 = t.keyPoints()[1];
    expect(segs.some((s) => Math.abs(s.to.lat - wp1.lat) < 1e-9 && Math.abs(s.to.lon - wp1.lon) < 1e-9)).toBe(true);
    const empty = new WeatherRibbon(new RouteTimeline(MissionTimelineBuilder.stops(m), 0, 10), new RouteWeather(t, []));
    expect(empty.segments('wind')).toEqual([]);
  });
});
