import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { Position } from 'geojson';
import { Mission, MissionStore, RouteDrawing, RouteResult, provideMapView } from '@rfmap/ui-kit';
import { coverageGrid, exampleMission, fixAltitude, fixTurnBack, routeResult } from '../mock/reference-case';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { RouteWeatherService } from '../core/route-weather.service';
import { MockApiHarness } from '../tests/mock-api-harness';
import { MapStageComponent } from './map-stage.component';

interface LimitShapes {
  radio: unknown;
  radioLabel: string;
  radioAnchor: Position | null;
  battery: { lat: number; lon: number; radiusM: number } | null;
  batteryLabel: string;
}

/** What the map stage draws, read from its view model (the MapLibre view itself is not rendered). */
describe('MapStageComponent drawing', () => {
  let fixture: ComponentFixture<MapStageComponent>;
  let store: MissionStore;

  const vm = () => fixture.componentInstance as unknown as { drawing: () => RouteDrawing; limitShapes: () => LimitShapes };
  const drawing = () => vm().drawing();

  function show(mission: Mission, result: RouteResult | null): void {
    store.newMission(mission);
    store.result.set(result);
  }

  beforeEach(() => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    TestBed.overrideComponent(MapStageComponent, { set: { template: '', imports: [] } });
    fixture = TestBed.createComponent(MapStageComponent);
    store = TestBed.inject(MissionStore);
  });

  afterEach(() => MockApiHarness.tearDown());

  it('sets the leg labels aside once the route is coloured by the weather, but not in a fix preview', async () => {
    const ws = TestBed.inject(FpvWorkspace);
    const wx = TestBed.inject(RouteWeatherService);
    const shown = () => (fixture.componentInstance as unknown as { weatherShown: () => boolean }).weatherShown();
    const m = exampleMission();
    show(m, null);
    expect(shown()).toBe(false);
    ws.weatherMode.set(true);
    TestBed.tick();
    // still loading: the route keeps its link colours and labels
    expect(shown()).toBe(false);
    for (let i = 0; i < 100 && wx.status() !== 'ready'; i++) await MockApiHarness.wait(100);
    expect(shown()).toBe(true);
    store.preview.set({ fix: { id: 'x' }, before: m, after: m });
    expect(shown()).toBe(false);
  });

  it('draws nothing without a mission', () => {
    expect(drawing()).toEqual({ pilot: null, waypoints: [], legs: [] });
  });

  it('draws pending legs and a silent return before a calculation', () => {
    show(exampleMission(), null);
    const d = drawing();
    expect(d.pilot?.ariaLabel).toBe('Pilot A, 2 m above ground');
    expect(d.pilot?.caption).toBe('Pilot · 2 m AGL');
    expect(d.waypoints.map((w) => w.ariaLabel)).toEqual(['Waypoint 1, 120 m above ground', 'Waypoint 2, 120 m above ground', 'Target B, 120 m above ground']);
    expect(d.legs.map((l) => l.status)).toEqual(['pending', 'pending', 'pending']);
    expect(d.legs[0].label).toMatch(/^\d+\.\d km$/);
    expect(d.markers).toEqual([]);
    expect(d.returnLeg).toMatchObject({ label: undefined, labelAt: 0.5 });
  });

  it('marks the blocked leg, the ridge and the blocked return after a calculation', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'normal'));
    const d = drawing();
    expect(d.legs.map((l) => l.status)).toEqual(['ok', 'ok', 'blocked']);
    expect(d.legs[2].label).toBe('3.9 km · V ✕');
    expect(d.legs[0].label).toBe('3.7 km · ✓');
    expect(d.pilot?.caption).toMatch(/^Pilot · \d+ \+ 2 m$/);
    expect(d.markers).toHaveLength(1);
    expect(d.markers![0]).toMatchObject({ kind: 'obstruction', symbol: '✕' });
    expect(d.markers![0].caption).toMatch(/^Ridge \d+ m$/);
    expect(d.returnLeg).toMatchObject({ label: 'Return · V ✕', labelAt: 0.78 });

    fixture.componentRef.setInput('compact', true);
    expect(drawing().returnLeg?.label).toBeUndefined();
  });

  it('shows the height of an ok leg flown off the default 120 m', () => {
    const m = exampleMission();
    const low = { ...m, waypoints: m.waypoints.map((w, i) => (i === 0 ? { ...w, aglM: 80 } : w)) };
    show(low, routeResult(low, 'normal'));
    expect(drawing().legs[0].label).toBe('3.7 km · 80 m ✓');
  });

  it('draws a missing-data leg as missing', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'missing-elevation'));
    expect(drawing().legs[2]).toEqual({ status: 'missing', label: '3.9 km · ? no data' });
  });

  it('draws a turn-back mission with a T marker and a turn-back label', () => {
    const base = exampleMission();
    const m = { ...base, waypoints: fixTurnBack(base).patch!['waypoints'] as Mission['waypoints'] };
    show(m, routeResult(m, 'normal'));
    const d = drawing();
    expect(d.waypoints.at(-1)).toMatchObject({ markerKind: 'turnback', symbol: 'T', ariaLabel: 'Turn-back point T' });
    expect(d.returnLeg?.label).toBe('Turn back · 4.2 km');
    expect(d.markers).toEqual([]);

    show({ ...m, returnToPilot: false }, null);
    expect(drawing().returnLeg).toBeNull();
  });

  it('previews a fix: changed legs as preview, the others with their calculated state', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'normal'));
    store.previewFix(fixAltitude(m));
    const d = drawing();
    expect(d.legs.map((l) => l.status)).toEqual(['ok', 'ok', 'preview']);
    expect(d.legs[2].label).toBe('3.9 km · 345 m');
    expect(d.legs[0].label).toBe('3.7 km · ✓');
    expect(d.markers).toEqual([]);
    expect(vm().limitShapes().battery).not.toBeNull();
  });

  it('previews against a result with missing data or failing links', () => {
    const m = exampleMission();
    const r = routeResult(m, 'missing-elevation');
    show(m, { ...r, legs: r.legs!.map((l, i) => (i === 0 ? { ...l, worst: { ...l.worst, control: { ...l.worst!.control!, reachable: false } } } : l)) });
    store.previewFix(fixAltitude(m));
    const d = drawing();
    expect(d.legs.map((l) => l.status)).toEqual(['blocked', 'ok', 'preview']);
    expect(d.legs[0].label).toBe('3.7 km · C ✕');
    store.cancelPreview();

    // a previewed waypoint the base result does not know yet is pending
    store.result.set({ ...r, legs: [] });
    store.preview.set({ fix: { id: 'x' }, before: m, after: m });
    expect(drawing().legs.map((l) => l.status)).toEqual(['pending', 'pending', 'pending']);
    store.preview.set(null);
    store.result.set(r);
    store.preview.set({ fix: { id: 'x' }, before: m, after: m });
    expect(drawing().legs[2].status).toBe('missing');
  });

  it('labels the radio limit at its far edge and the battery return limit', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'normal'));
    expect(vm().limitShapes()).toMatchObject({ radio: null, radioLabel: '', radioAnchor: null });
    store.coverage.set({ control: coverageGrid(m, 'control') });
    const s = vm().limitShapes();
    expect(s.radioLabel).toMatch(/^Control link limit · \d+\.\d km$/);
    expect(s.radioAnchor).toHaveLength(2);
    expect(s.battery).toEqual({ lat: m.pilot.lat, lon: m.pilot.lon, radiusM: routeResult(m, 'normal').battery!.rangeM! / 2 });
    expect(s.batteryLabel).toBe('Battery return limit · 5.1 km');

    store.coverage.set({ video: coverageGrid(m, 'video') });
    store.update((x) => ({ ...x, evaluate: 'video' }));
    expect(vm().limitShapes().radioLabel).toMatch(/^Video link limit/);
  });

  it('hides what was drawn from the old pilot position after the pilot moves', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'normal'));
    store.coverage.set({ control: coverageGrid(m, 'control') });
    store.setPilot({ lat: m.pilot.lat + 0.01, lon: m.pilot.lon });
    expect(vm().limitShapes()).toMatchObject({ radio: null, battery: null, batteryLabel: '' });
    expect(drawing().markers).toEqual([]);
    expect(drawing().pilot?.caption).toBe('Pilot · 2 m AGL');
    store.undoMove();
    expect(vm().limitShapes().battery).not.toBeNull();
    expect(drawing().markers).toHaveLength(1);
  });

  it('lets points be dragged on the desktop map only while the mission can be edited', () => {
    const draggable = () => (fixture.componentInstance as unknown as { pointsDraggable: () => boolean }).pointsDraggable();
    const m = exampleMission();
    show(m, null);
    expect(draggable()).toBe(true);
    fixture.componentRef.setInput('interactiveEdit', false);
    expect(draggable()).toBe(false);
    fixture.componentRef.setInput('interactiveEdit', true);
    store.role.set('view');
    expect(draggable()).toBe(false);
  });

  it('moves the pilot of the current mission on a map click and keeps the route', () => {
    const place = (p: { lat: number; lon: number }) => (fixture.componentInstance as unknown as { placePilot: (p: { lat: number; lon: number }) => void }).placePilot(p);
    const ws = (fixture.componentInstance as unknown as { ws: { placing: { set: (v: string | null) => void; (): string | null } } }).ws;
    const m = exampleMission();
    show(m, null);
    ws.placing.set('pilot');
    place({ lat: 48.6, lon: 35.3 });
    expect(store.mission()!.pilot).toMatchObject({ lat: 48.6, lon: 35.3, aglM: m.pilot.aglM });
    expect(store.mission()!.waypoints).toEqual(m.waypoints);
    expect(ws.placing()).toBeNull();
  });

  it('starts a new mission from the first pilot click, then adds points', () => {
    const stage = fixture.componentInstance as unknown as {
      placePilot: (p: { lat: number; lon: number }) => void;
      cancelPlacing: () => void;
      ws: { placing: { set: (v: string | null) => void; (): string | null }; pendingMission: { set: (m: Mission | null) => void; (): Mission | null } };
    };
    stage.ws.pendingMission.set({ ...exampleMission(), waypoints: [] });
    stage.ws.placing.set('pilot');
    stage.cancelPlacing();
    expect(stage.ws.placing()).toBe('pilot');
    stage.placePilot({ lat: 48.6, lon: 35.3 });
    expect(store.mission()!.pilot).toMatchObject({ lat: 48.6, lon: 35.3 });
    expect(stage.ws.pendingMission()).toBeNull();
    expect(stage.ws.placing()).toBe('waypoints');

    stage.ws.placing.set('pilot');
    stage.cancelPlacing();
    expect(stage.ws.placing()).toBeNull();
  });

  it('accepts line and polygon limits and ignores other geometry', () => {
    const m = exampleMission();
    show(m, null);
    const grid = coverageGrid(m, 'control');
    const p = m.pilot;
    const ring = [[p.lon - 0.05, p.lat], [p.lon, p.lat + 0.05], [p.lon + 0.05, p.lat], [p.lon - 0.05, p.lat]];
    const label = (limitPolygon: unknown) => {
      store.coverage.set({ control: { ...grid, limitPolygon: limitPolygon as never } });
      return vm().limitShapes();
    };
    expect(label({ type: 'Polygon', coordinates: [ring] }).radioAnchor).toEqual(ring[0]);
    expect(label({ type: 'LineString', coordinates: ring }).radioLabel).toMatch(/km$/);
    expect(label({ type: 'MultiLineString', coordinates: [ring] }).radioLabel).toMatch(/km$/);
    expect(label({ type: 'Point', coordinates: ring[0] })).toMatchObject({ radioLabel: '', radioAnchor: null });
  });

  it('keeps drawing after the map is ready and cleans up on destroy', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'normal'));
    fixture.componentInstance.onReady(null as never);
    fixture.detectChanges();
    store.coverage.set({ control: coverageGrid(m, 'control') });
    fixture.detectChanges();
    expect(drawing().legs).toHaveLength(3);
    fixture.destroy();
  });
});
