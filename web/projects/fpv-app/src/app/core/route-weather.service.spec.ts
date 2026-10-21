import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MissionStore, RfmapApiClient, RouteTimeline, WeatherPoint, provideMapView } from '@rfmap/ui-kit';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { FpvDataService } from './fpv-data.service';
import { FALLBACK_SPEED_MPS, FlightClock, ForecastCache, LimitedRunner, MissionTimelineBuilder, RouteWeatherService } from './route-weather.service';

/** Weather along the route from the dev mock backend's /weather/point. */
describe('RouteWeatherService', () => {
  let store: MissionStore;
  let wx: RouteWeatherService;

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    store = TestBed.inject(MissionStore);
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    wx = TestBed.inject(RouteWeatherService);
  });

  afterEach(() => MockApiHarness.tearDown());

  async function loaded(): Promise<void> {
    TestBed.tick();
    for (let i = 0; i < 100 && wx.status() !== 'ready' && wx.status() !== 'error'; i++) await MockApiHarness.wait(100);
  }

  it('stays idle without a route', () => {
    TestBed.tick();
    expect(wx.status()).toBe('idle');
    expect(wx.keyPoints()).toEqual([]);
    expect(wx.window()).toBe('');
  });

  it('reads the weather at every route point at its arrival time', async () => {
    store.newMission(exampleMission());
    await loaded();
    expect(wx.status()).toBe('ready');
    const k = wx.keyPoints();
    expect(k.map((p) => p.name)).toEqual(['A', 'WP1', 'WP2', 'B', 'A']);
    expect(k[0].alongTrack).toBe('');
    expect(k[1].alongTrack).toMatch(/^(head|tail) \d+\.\d m\/s$/);
    expect(k[0].temperature).toMatch(/°C$/);
    expect(k.every((p) => p.reading !== null)).toBe(true);
    // the mock freshens the wind towards the north-east, so B is windier than A
    expect(k[3].reading!.windSpeedMps!).toBeGreaterThan(k[0].reading!.windSpeedMps!);
    expect(wx.summary().map((s) => s.key)).toEqual(['wind', 'rain', 'temperature']);
    expect(wx.source()?.model).toContain('Open-Meteo');
    expect(wx.window()).toMatch(/^\d\d:\d\d–\d\d:\d\d$/);
    expect(wx.usedInCalculation()).toBe(true);
    expect(wx.speedMps()).toBe(10);
  });

  it('says so when no forecast covers the flight time', async () => {
    const m = exampleMission();
    store.newMission({ ...m, time: { mode: 'planned', utc: new Date(Date.now() + 40 * 86400e3).toISOString() } });
    await loaded();
    expect(wx.beyondForecast()).toBe(true);
    expect(wx.summary()).toEqual([]);
    expect(wx.source()).toBeNull();
  });

  it('reports an error when every request fails, and loads again on refresh', async () => {
    const api = TestBed.inject(RfmapApiClient);
    const spy = vi.spyOn(api, 'weatherPoint').mockRejectedValue(new Error('forecast service down'));
    store.newMission(exampleMission());
    await loaded();
    expect(wx.status()).toBe('error');
    expect(wx.error()).toContain('forecast service down');
    spy.mockRestore();
    wx.refresh();
    await loaded();
    expect(wx.status()).toBe('ready');
  });

  it('uses an assumed speed without a drone and notes weather left out of the calculation', async () => {
    const m = exampleMission();
    store.newMission({ ...m, drone: undefined, modules: {} });
    await loaded();
    expect(wx.speedMps()).toBe(FALLBACK_SPEED_MPS);
    expect(wx.usedInCalculation()).toBe(false);
  });

  it('moves "now" on when a mission flown now changes', async () => {
    store.newMission(exampleMission());
    await loaded();
    const before = wx.timeline()!.departureMs;
    vi.setSystemTime(Date.now() + 2 * 3600e3);
    store.update((m) => ({ ...m }));
    TestBed.tick();
    const after = wx.timeline()!.departureMs;
    expect(after - before).toBeGreaterThanOrEqual(2 * 3600e3 - 60e3);
    expect(after % 60e3).toBe(0);
  });

  it('drops a load overtaken by a route change', async () => {
    const answer: WeatherPoint = { tier: 'hourly', temperatureC: 3 };
    let release: (w: WeatherPoint) => void = () => undefined;
    const gate = new Promise<WeatherPoint>((resolve) => (release = resolve));
    const spy = vi.spyOn(TestBed.inject(RfmapApiClient), 'weatherPoint').mockReturnValue(gate);
    store.newMission(exampleMission());
    TestBed.tick();
    await MockApiHarness.wait(500);
    expect(spy).toHaveBeenCalled();
    store.update((m) => ({ ...m, waypoints: m.waypoints.slice(0, -1) }));
    TestBed.tick();
    release(answer);
    await MockApiHarness.wait(100);
    expect(wx.status()).toBe('loading');
    expect(wx.route()).toBeNull();
    spy.mockRestore();
    await loaded();
    expect(wx.status()).toBe('ready');
    expect(wx.route()).not.toBeNull();
  });

  it('rates the temperature by both ends of its range', async () => {
    let calls = 0;
    vi.spyOn(TestBed.inject(RfmapApiClient), 'weatherPoint').mockImplementation(async () => ({ tier: 'hourly', temperatureC: calls++ === 0 ? 20 : 28 }));
    store.newMission(exampleMission());
    await loaded();
    expect(wx.summary().find((s) => s.key === 'temperature')?.level).toBe('notable');
  });

  it('gives the hours around a point for the briefing', async () => {
    store.newMission(exampleMission());
    await loaded();
    const hours = await MockApiHarness.settle(wx.hourly(wx.keyPoints()[0].point, 1));
    expect(hours).toHaveLength(3);
    expect(hours[1].timeMs - hours[0].timeMs).toBe(3600e3);
    expect(hours.every((h) => h.weather?.windSpeedMps !== undefined)).toBe(true);
  });
});

describe('route weather helpers', () => {
  it('caches forecasts by rounded place and hour, and forgets failures', async () => {
    const answer: WeatherPoint = { tier: 'hourly', temperatureC: 3 };
    const fetch = vi.fn().mockRejectedValueOnce(new Error('down')).mockResolvedValue(answer);
    const cache = new ForecastCache(fetch);
    const t = Date.UTC(2026, 9, 21, 12, 20);
    await expect(cache.get(48.5301, 35.2001, t)).rejects.toThrow('down');
    await expect(cache.get(48.5301, 35.2001, t)).resolves.toBe(answer);
    await cache.get(48.5299, 35.1999, t + 5 * 60e3);
    expect(fetch).toHaveBeenCalledTimes(2);
    expect(fetch).toHaveBeenLastCalledWith(48.53, 35.2, '2026-10-21T12:00:00.000Z');
    cache.clear();
    await cache.get(48.53, 35.2, t);
    expect(fetch).toHaveBeenCalledTimes(3);
  });

  it('runs jobs a few at a time and keeps their order', async () => {
    let inFlight = 0;
    let peak = 0;
    const job = async (n: number) => {
      inFlight++;
      peak = Math.max(peak, inFlight);
      await Promise.resolve();
      inFlight--;
      if (n === 3) throw new Error('three');
      return n * 2;
    };
    const out = await new LimitedRunner(2).run([1, 2, 3, 4, 5], job);
    expect(peak).toBe(2);
    expect(out.map((r) => (r.status === 'fulfilled' ? r.value : 'x'))).toEqual([2, 4, 'x', 8, 10]);
  });

  it('builds the stops, departure and samples of a mission', () => {
    const m = exampleMission();
    expect(MissionTimelineBuilder.stops(m).map((s) => s.name)).toEqual(['A', 'WP1', 'WP2', 'B', 'A']);
    expect(MissionTimelineBuilder.stops({ ...m, returnToPilot: false })).toHaveLength(4);
    expect(MissionTimelineBuilder.departureMs({ ...m, time: { mode: 'planned', utc: '2026-10-22T09:00:00Z' } }, 0)).toBe(Date.parse('2026-10-22T09:00:00Z'));
    expect(MissionTimelineBuilder.departureMs(m, 42)).toBe(42);
    const t = new RouteTimeline(MissionTimelineBuilder.stops(m), 0, 10);
    const pts = MissionTimelineBuilder.samplePoints(t);
    expect(pts.filter((p) => p.name).map((p) => p.name)).toEqual(['A', 'WP1', 'WP2', 'B', 'A']);
    expect(pts.length).toBeGreaterThan(10);
    expect(pts.every((p, i) => i === 0 || p.alongM >= pts[i - 1].alongM)).toBe(true);
  });

  it('formats flight times', () => {
    expect(FlightClock.hm(new Date(2026, 9, 21, 15, 7).getTime())).toBe('15:07');
    expect(FlightClock.day(new Date(2026, 9, 21, 15, 7).getTime())).toMatch(/^Wed,? 21 Oct$/);
  });
});
