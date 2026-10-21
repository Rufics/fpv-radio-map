import type { Map as MlMap } from 'maplibre-gl';
import { describe, expect, it, vi } from 'vitest';
import { MINUS, RfFormat } from '../format/rf-format';
import { WeatherPoint } from '../models/api.models';
import { destinationPoint, initialBearingDeg } from '../tools/geo';
import { RouteTimeline } from './route-timeline';
import { PolylineHitTest, RouteWeatherLayer } from './route-weather-layer';
import { RouteWeather, WeatherSample } from './route-weather';
import { WeatherScale } from './weather-scale';
import { Wind } from './wind';

// A square-ish route near Dnipro: A → 2 km north → 2 km east → back to A.
const A = { name: 'A', lat: 48.5, lon: 35.2 };
const north = { name: 'WP1', ...destinationPoint(A.lat, A.lon, 0, 2000) };
const east = { name: 'B', ...destinationPoint(north.lat, north.lon, 90, 2000) };
const DEPART = Date.UTC(2026, 9, 21, 12, 0, 0);
const timeline = () => new RouteTimeline([A, north, east, { ...A, name: 'A' }], DEPART, 10);

const forecast = (over: Partial<WeatherPoint> = {}): WeatherPoint => ({
  tier: 'hourly',
  model: 'Open-Meteo icon_d2',
  temperatureC: 10,
  windSpeedMps: 6,
  windFromDeg: 0,
  precipitationMmPerHour: 0,
  relativeHumidity: 70,
  ...over,
});

describe('Wind', () => {
  it('names compass points and the direction the wind blows to', () => {
    expect(Wind.compass(250)).toBe('WSW');
    expect(Wind.compass(359)).toBe('N');
    expect(Wind.compass(-90)).toBe('W');
    expect(Wind.towardsDeg(250)).toBe(70);
  });

  it('splits the wind into a tailwind (+) or a headwind (−) along the track', () => {
    // wind from the north, flying north: straight on the nose
    expect(Wind.alongTrackMps(6, 0, 0)).toBeCloseTo(-6);
    // flying south: straight behind
    expect(Wind.alongTrackMps(6, 0, 180)).toBeCloseTo(6);
    // crosswind adds nothing along the track
    expect(Wind.alongTrackMps(6, 0, 90)).toBeCloseTo(0);
  });

  it('interpolates directions along the shorter arc', () => {
    expect(Wind.lerpDeg(350, 10, 0.5)).toBeCloseTo(0);
    expect(Wind.lerpDeg(10, 350, 0.25)).toBeCloseTo(5);
  });
});

describe('RouteTimeline', () => {
  it('times each route point at the cruise speed from the departure', () => {
    const k = timeline().keyPoints();
    expect(k.map((p) => p.name)).toEqual(['A', 'WP1', 'B', 'A']);
    expect(k[1].alongM).toBeCloseTo(2000, 0);
    // 2 km at 10 m/s is 200 s
    expect(k[1].etaMs - DEPART).toBeCloseTo(200_000, -2);
    expect(k[2].leg).toBe(1);
    expect(k[0].leg).toBe(0);
  });

  it('knows the leg and track at any distance', () => {
    const t = timeline();
    expect(t.legAt(1000)).toBe(0);
    expect(t.trackOf(0)).toBeCloseTo(0, 0);
    expect(t.trackOf(1)).toBeCloseTo(90, 0);
    expect(t.pointAt(3000).leg).toBe(1);
    expect(t.pointAt(-5).alongM).toBe(0);
    expect(t.pointAt(1e9).alongM).toBeCloseTo(t.lengthM);
    expect(t.legStartM(1)).toBeCloseTo(2000, 0);
    expect(t.arrivalMs).toBeGreaterThan(DEPART);
  });

  it('samples evenly, never more than asked', () => {
    const s = timeline().samples(500, 6);
    expect(s).toHaveLength(6);
    expect(s[0].alongM).toBe(0);
    expect(s.at(-1)!.alongM).toBeCloseTo(timeline().lengthM);
  });

  it('handles a route with one point and refuses a zero speed', () => {
    const one = new RouteTimeline([A], DEPART, 10);
    expect(one.keyPoints()).toHaveLength(1);
    expect(one.samples(500, 10)).toHaveLength(1);
    expect(one.pointAt(10).etaMs).toBe(DEPART);
    expect(() => new RouteTimeline([A], DEPART, 0)).toThrow(RangeError);
  });

  it('computes bearings clockwise from north', () => {
    expect(initialBearingDeg(A, north)).toBeCloseTo(0, 0);
    expect(initialBearingDeg(north, A)).toBeCloseTo(180, 0);
  });
});

describe('RouteWeather', () => {
  const samples = (): WeatherSample[] => {
    const pts = timeline().samples(1000, 7);
    return pts.map((p, i) => ({ point: p, weather: forecast({ temperatureC: 10 - i, windSpeedMps: 4 + i, precipitationMmPerHour: i >= 4 ? 0.5 : 0 }) }));
  };

  it('interpolates between samples and computes the wind along the track', () => {
    const rw = new RouteWeather(timeline(), samples());
    const step = timeline().lengthM / 6;
    const r = rw.readingAt(step / 2)!;
    expect(r.temperatureC).toBeCloseTo(9.5, 5);
    // wind from the north on the northbound leg: headwind
    expect(r.alongTrackMps!).toBeLessThan(0);
    expect(rw.readings()).toHaveLength(7);
  });

  it('summarises headwind, rain and ranges', () => {
    const rw = new RouteWeather(timeline(), samples());
    expect(rw.strongestHeadwind()?.leg).toBe(0);
    expect(rw.firstRain()?.alongM).toBeGreaterThan(3000);
    expect(rw.max('windSpeedMps')).toBe(10);
    expect(rw.min('temperatureC')).toBe(4);
    expect(rw.nearestForecast(0)?.model).toBe('Open-Meteo icon_d2');
  });

  it('colours the route only with a forecast at two samples or more', () => {
    const pts = timeline().samples(1000, 3);
    const one = pts.map((p, i) => ({ point: p, weather: i === 0 ? { tier: 'hourly' as const, temperatureC: 4 } : { tier: 'none' as const } }));
    expect(new RouteWeather(timeline(), one).empty).toBe(false);
    expect(new RouteWeather(timeline(), one).colourable).toBe(false);
    const two = pts.map((p, i) => ({ point: p, weather: i < 2 ? { tier: 'hourly' as const, temperatureC: 4 } : { tier: 'none' as const } }));
    expect(new RouteWeather(timeline(), two).colourable).toBe(true);
  });

  it('leaves out samples beyond the forecast and makes nothing up', () => {
    const pts = timeline().samples(1000, 3);
    const rw = new RouteWeather(timeline(), pts.map((p) => ({ point: p, weather: { tier: 'none', model: 'none' } })));
    expect(rw.empty).toBe(true);
    expect(rw.colourable).toBe(false);
    expect(rw.readingAt(100)).toBeNull();
    expect(rw.strongestHeadwind()).toBeNull();
    expect(rw.max('temperatureC')).toBeNull();
    expect(rw.nearestForecast(0)).toBeNull();
    expect(RouteWeather.hasForecast(null)).toBe(false);
  });

  it('blends partial forecasts and reads before the first sample', () => {
    const pts = timeline().samples(1000, 3);
    const rw = new RouteWeather(timeline(), [
      { point: pts[1], weather: forecast({ windFromDeg: undefined, temperatureC: undefined }) },
      { point: pts[2], weather: forecast({ temperatureC: 4 }) },
    ]);
    expect(rw.readingAt(0)!.alongTrackMps).toBeUndefined();
    const mid = rw.readingAt((pts[1].alongM + pts[2].alongM) / 2)!;
    expect(mid.temperatureC).toBe(4);
    expect(mid.windFromDeg).toBe(0);
    // nearer the sample without a direction, the other's direction still holds
    expect(rw.readingAt(pts[1].alongM + 1)!.windFromDeg).toBe(0);
  });

  it('holds the last forecast past the end of the covered samples', () => {
    const t = timeline();
    const pts = t.samples(1000, 4);
    const rw = new RouteWeather(t, [
      { point: pts[0], weather: forecast({ temperatureC: 1 }) },
      { point: pts[1], weather: forecast({ temperatureC: 2 }) },
      { point: pts[3], weather: { tier: 'none' } },
    ]);
    expect(rw.readingAt(t.lengthM)!.temperatureC).toBe(2);
  });
});

describe('WeatherScale', () => {
  it('classes values with colours and levels', () => {
    expect(WeatherScale.level('headwind', -8)).toBe('strong');
    expect(WeatherScale.level('headwind', -4)).toBe('notable');
    expect(WeatherScale.level('headwind', -2)).toBe('calm');
    expect(WeatherScale.level('headwind', 3)).toBe('calm');
    expect(WeatherScale.level('wind', 10)).toBe('notable');
    expect(WeatherScale.level('wind', 13)).toBe('strong');
    expect(WeatherScale.level('temperature', -3)).toBe('notable');
    expect(WeatherScale.level('rain', undefined)).toBe('calm');
    expect(WeatherScale.color('rain', undefined)).toBe(WeatherScale.NO_DATA);
    expect(WeatherScale.color('wind', 2)).not.toBe(WeatherScale.color('wind', 12));
    expect(WeatherScale.legend('temperature')).toHaveLength(4);
    expect(WeatherScale.title('headwind')).toBe('Head / tail');
  });

  it('reads and formats every metric of a reading', () => {
    const r = { alongM: 0, etaMs: 0, lat: 0, lon: 0, leg: 0, windSpeedMps: 9, alongTrackMps: -4, temperatureC: 18, precipitationMmPerHour: 1.2 };
    expect(WeatherScale.METRICS.map((m) => WeatherScale.format(m, WeatherScale.value(m, r)))).toEqual(['head 4.0 m/s', '9.0 m/s', '18 °C', '1.2 mm/h']);
    expect(WeatherScale.METRICS.map((m) => WeatherScale.level(m, WeatherScale.value(m, r)))).toEqual(['notable', 'notable', 'calm', 'strong']);
  });

  it('formats values with units', () => {
    expect(WeatherScale.format('headwind', -5.42)).toBe('head 5.4 m/s');
    expect(WeatherScale.format('headwind', 3)).toBe('tail 3.0 m/s');
    expect(WeatherScale.format('temperature', -2.4)).toBe(`${MINUS}2 °C`);
    expect(WeatherScale.format('rain', undefined)).toBe('—');
    expect(RfFormat.format(1.18312, 'kgm3')).toBe('1.183 kg/m³');
    expect(RfFormat.format(-3.1, 'mps+')).toBe(`${MINUS}3.1 m/s`);
    expect(WeatherScale.value('wind', { alongM: 0, etaMs: 0, lat: 0, lon: 0, leg: 0, windSpeedMps: 3 })).toBe(3);
  });
});

describe('PolylineHitTest', () => {
  const line = new PolylineHitTest([
    { x: 0, y: 0 },
    { x: 100, y: 0 },
    { x: 100, y: 100 },
  ]);

  it('finds the nearest place on the line within the tolerance', () => {
    const hit = line.nearest({ x: 50, y: 4 }, 10)!;
    expect(hit.segment).toBe(0);
    expect(hit.t).toBeCloseTo(0.5);
    expect(hit.distancePx).toBeCloseTo(4);
    expect(line.nearest({ x: 103, y: 60 }, 10)!.segment).toBe(1);
  });

  it('ignores a cursor far from the line and zero-length segments', () => {
    expect(line.nearest({ x: 50, y: 50 }, 10)).toBeNull();
    expect(new PolylineHitTest([{ x: 5, y: 5 }, { x: 5, y: 5 }]).nearest({ x: 6, y: 5 }, 2)!.t).toBe(0);
  });
});

describe('RouteWeatherLayer', () => {
  it('draws one coloured feature per segment and updates the source', () => {
    const setData = vi.fn().mockReturnValue(Promise.resolve());
    const map = { getSource: () => ({ setData }), addSource: vi.fn(), addLayer: vi.fn(), getLayer: () => undefined } as unknown as MlMap;
    const layer = new RouteWeatherLayer();
    layer.attach(map, undefined);
    layer.setSegments([{ from: A, to: north, color: '#FF6B5B' }]);
    const data = setData.mock.calls[0][0];
    expect(data.features).toHaveLength(1);
    expect(data.features[0].properties.color).toBe('#FF6B5B');
    expect(layer.styleLayerIds()).toEqual(['route-weather-casing', 'route-weather']);
  });
});
