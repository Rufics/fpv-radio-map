import { describe, expect, it } from 'vitest';
import { fpvCoreData } from '../core/fpv-core';
import { fpvCoreMock } from './fpv-core-mock';
import { exampleMission, routeResult } from './reference-case';

describe('fpv.core mock (compare + export)', () => {
  it('compares B = A + a 3 dB better ground antenna point by point', () => {
    const m = exampleMission();
    m.modules = { ...m.modules, 'fpv.core': { compare: { name: 'Patch', patch: { links: { video: { groundAntenna: 'patch-8dbi-915' } } } } } };
    const out = fpvCoreMock(m);
    const v = out.data.compare?.links?.video;
    expect(out.data.compare?.aligned).toBe(true);
    expect(v?.deltaWorstDb).toBeCloseTo(3, 9);
    expect(v?.pointsWorse).toBe(0);
    expect(v?.pointsBetter).toBe(v?.points?.length);
    expect(out.data.compare?.links?.control?.deltaWorstDb).toBeCloseTo(0, 9);
  });

  it('is not aligned when B moves the route, and then has no points', () => {
    const m = exampleMission();
    m.modules = { 'fpv.core': { compare: { patch: { pilot: { lat: m.pilot.lat + 0.01 } } } } };
    const v = fpvCoreMock(m).data.compare;
    expect(v?.aligned).toBe(false);
    expect(v?.links?.video?.points).toBeUndefined();
  });

  it('exports KML with the requested altitude mode, readable through RouteResult.modules', () => {
    const m = exampleMission();
    m.modules = { 'fpv.core': { export: ['kml', 'gpx'], kmlAltitude: 'absolute', exportStepM: 200 } };
    const r = routeResult(m, 'normal');
    r.modules = { ...(r.modules ?? {}), 'fpv.core': fpvCoreMock(m) };
    const files = fpvCoreData(r)?.export;
    expect(files?.kml?.mediaType).toBe('application/vnd.google-earth.kml+xml');
    expect(files?.kml?.content).toContain('<altitudeMode>absolute</altitudeMode>');
    expect(files?.gpx?.content).toContain('<gpx');
  });
});
