import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { Mission, MissionStore, RouteResult } from '@rfmap/ui-kit';
import { exampleMission, fixAltitude, fixTurnBack, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { FpvDataService } from './fpv-data.service';
import { FpvWorkspace, endsAtTurnBack, pointName } from './fpv-workspace.service';

/** The FPV view model over the reference case answers of the dev mock. */
describe('FpvWorkspace', () => {
  let store: MissionStore;
  let ws: FpvWorkspace;

  function show(mission: Mission, result: RouteResult | null): void {
    store.newMission(mission);
    store.result.set(result);
  }

  function turnBackMission(): Mission {
    const m = exampleMission();
    return { ...m, waypoints: fixTurnBack(m).patch!['waypoints'] as Mission['waypoints'] };
  }

  beforeEach(async () => {
    MockApiHarness.setUp();
    store = TestBed.inject(MissionStore);
    ws = TestBed.inject(FpvWorkspace);
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
  });

  afterEach(() => MockApiHarness.tearDown());

  it('names points and spots a turn-back mission', () => {
    const m = exampleMission();
    expect(pointName(m, -1)).toBe('A');
    expect(pointName(m, 0)).toBe('WP1');
    expect(pointName(m, 2)).toBe('B');
    expect(pointName({ ...m, pilot: { ...m.pilot, label: undefined }, waypoints: [{ ...m.waypoints[0], label: undefined }] }, -1)).toBe('A');
    expect(endsAtTurnBack(m)).toBe(false);
    expect(endsAtTurnBack(turnBackMission())).toBe(true);
  });

  it('has no pills before a calculation', () => {
    show(exampleMission(), null);
    expect(ws.pills()).toEqual([]);
    expect(ws.verdict()).toBeNull();
    expect(ws.allClearText()).toBeNull();
  });

  it('shows a blocked video pill and a short battery for the reference case', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'normal'));
    const [control, video, battery] = ws.pills();
    expect(control).toMatchObject({ key: 'control', tone: 'ok' });
    expect(control.text).toMatch(/^C ✓ \+/);
    expect(video).toMatchObject({ key: 'video', tone: 'blocked', text: 'V ✕ −3.74 dB' });
    expect(video.title).toContain('Worst video margin along the route');
    expect(battery).toMatchObject({ key: 'battery', tone: 'warn' });
    expect(battery.text).toMatch(/^Battery ! .* km short$/);
    expect(ws.verdict()).toMatchObject({ legIndex: 3, title: 'Leg 3 · WP2 → B', badge: 'V ✕', link: 'video' });
    expect(ws.verdict()?.fix?.id).toBe('leg3-345m-agl');
    expect(ws.batteryCard()?.suggestion).toContain('turn back at T');
  });

  it('on an autonomous route a lost link is a warning, not the verdict', () => {
    const m: Mission = { ...exampleMission(), flightMode: 'autonomous' };
    show(m, routeResult(m, 'normal'));
    expect(ws.pills().find((p) => p.key === 'video')).toMatchObject({ tone: 'warn', text: 'V ! −3.74 dB' });
    expect(ws.legRows()[2]).toMatchObject({ mark: 'V !', tone: 'warn', failing: [] });
    expect(ws.legRows()[2].markLabel).toContain('autopilot flies on');
    expect(ws.verdict()).toBeNull();
  });

  it('says where an autonomous route loses a link instead of the all-clear', () => {
    const m: Mission = { ...exampleMission(), flightMode: 'autonomous' };
    show(m, routeResult(m, 'normal'));
    expect(ws.linkGapText()).toBe('The autopilot flies the whole route, but the pilot cannot take over where a link is lost: video on leg 3 · WP2 → B; video on leg 4 · B → A, return.');
    expect(ws.allClearText()).toBeNull();
    show(exampleMission(), routeResult(exampleMission(), 'normal'));
    expect(ws.linkGapText()).toBeNull();
  });

  it('lists the flight checks of a fixed-wing drone and marks the legs they name', () => {
    const m: Mission = { ...exampleMission(), drone: 'fw-1800-mapper', modules: { 'fpv.airframe': {} } };
    const r = routeResult(m, 'normal');
    expect(ws.flightChecks()).toEqual([]);
    show(m, r);
    expect(ws.flightChecks()).toEqual([expect.objectContaining({ tone: 'warn', module: 'Fixed-wing flight envelope', where: 'Whole route', legIndex: null })]);
    expect(ws.flightChecks()[0].text).toContain('a gust in a turn can stall it');

    const strike = { module: 'fpv.airframe', severity: 'blocked' as const, leg: 0, text: 'Fixed-wing: climbing at most 8°, the plane flies into terrain or clutter.' };
    const climb = { module: 'fpv.airframe', severity: 'warn' as const, leg: 1, text: 'Fixed-wing: the plane reaches WP2 40 m below the planned height.' };
    show(m, { ...r, legs: r.legs!.map((l, i) => (i < 2 ? { ...l, reasons: [...(l.reasons ?? []), i === 0 ? strike : climb] } : l)) });
    expect(ws.flightChecks().map((c) => [c.tone, c.where])).toEqual([['blocked', 'Leg 1 · A → WP1'], ['warn', 'Leg 2 · WP1 → WP2'], ['warn', 'Whole route']]);
    expect(ws.legRows()[0]).toMatchObject({ mark: '✕', tone: 'blocked', failing: [] });
    expect(ws.legRows()[1]).toMatchObject({ mark: '!', tone: 'warn' });
  });

  it('lets a blocking flight check outrank a lost link and the all-clear', () => {
    const m: Mission = { ...exampleMission(), flightMode: 'autonomous', drone: 'fw-1800-mapper', modules: { 'fpv.airframe': {} } };
    const r = routeResult(m, 'normal');
    const strike = { module: 'fpv.airframe', severity: 'blocked' as const, leg: 2, text: 'Fixed-wing: climbing at most 8°, the plane flies into terrain or clutter.' };
    show(m, { ...r, legs: r.legs!.map((l, i) => (i === 2 ? { ...l, reasons: [...(l.reasons ?? []), strike] } : l)) });
    expect(ws.legRows()[2]).toMatchObject({ mark: '✕', tone: 'blocked' });
    expect(ws.legRows()[3]).toMatchObject({ mark: 'V !', tone: 'warn' });

    const none: Mission = { ...m, flightMode: 'manual', evaluate: 'none' };
    const clear = routeResult(none, 'normal');
    show(none, { ...clear, legs: clear.legs!.map((l, i) => ({ ...l, status: 'ok' as const, worst: {}, reasons: i === 2 ? [strike] : [] })) });
    expect(ws.allClearText()).toBeNull();
  });

  it('names the all-clear after the ground when no link is checked', () => {
    const m: Mission = { ...exampleMission(), evaluate: 'none', drone: 'fw-1800-mapper', modules: { 'fpv.airframe': {} } };
    const r = routeResult(m, 'normal');
    show(m, { ...r, legs: r.legs!.map((l) => ({ ...l, status: 'ok' as const, reasons: [], worst: {} })) });
    expect(ws.allClearTitle()).toBe('Route clear of the ground');
    expect(ws.allClearText()).toBe('No radio link is checked in this mode; the route stays clear of the ground.');
    show(exampleMission(), null);
    expect(ws.allClearTitle()).toBe('All flown legs reachable by radio');
  });

  it('marks pills of a route with missing elevation as part missing', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'missing-elevation'));
    const pills = ws.pills();
    const control = pills.find((p) => p.key === 'control')!;
    expect(control.tone).toBe('warn');
    expect(control.text).toMatch(/^C \? .* · part missing$/);
    expect(ws.missingLeg()?.number).toBe(3);
    expect(ws.missingAreas().length).toBeGreaterThan(0);
    const rows = ws.legRows();
    expect(rows[2]).toMatchObject({ mark: '? no data', tone: 'warn' });
  });

  it('says a link is not calculated when no leg has a margin for it', () => {
    const m = exampleMission();
    const r = routeResult(m, 'normal');
    show(m, { ...r, legs: r.legs!.map((l) => ({ ...l, worst: { control: l.worst?.control } })) });
    expect(ws.pills().find((p) => p.key === 'video')).toEqual({ key: 'video', tone: 'warn', text: 'V ? not calculated' });
  });

  it('only shows pills for evaluated links', () => {
    const m = { ...exampleMission(), evaluate: 'control' as const };
    show(m, routeResult(m, 'normal'));
    expect(ws.pills().map((p) => p.key)).toEqual(['control', 'battery']);
    expect(ws.heatLink()).toBe('control');
    ws.coverageLink.set('video');
    expect(ws.heatLink()).toBe('control');
  });

  it('flags a failed battery module as not checked', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'module-failed'));
    expect(ws.pills().at(-1)).toMatchObject({ key: 'battery', tone: 'warn', text: 'Battery ? not checked' });
    expect(ws.batteryFailure()?.module).toContain('fpv.battery');
  });

  it('has a battery pill for a route that fits, with and without a turn back', () => {
    const m = turnBackMission();
    show(m, routeResult(m, 'normal'));
    expect(ws.pills().at(-1)).toEqual({ key: 'battery', tone: 'ok', text: 'Battery ✓ turn back at T' });
    expect(ws.verdict()).toBeNull();
    expect(ws.allClearText()).toContain('The flight ends at T');

    const plain = exampleMission();
    const r = routeResult(plain, 'normal');
    show(plain, { ...r, battery: { ...r.battery!, shortfallM: 0 } });
    expect(ws.pills().at(-1)).toEqual({ key: 'battery', tone: 'ok', text: 'Battery ✓ 10.2 km range' });

    show(plain, { ...r, battery: undefined });
    expect(ws.pills().map((p) => p.key)).toEqual(['control', 'video']);
  });

  it('lists the legs with the return leg last', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'normal'));
    const rows = ws.legRows();
    expect(rows.map((r) => r.name)).toEqual(['1 · A → WP1', '2 · WP1 → WP2', '3 · WP2 → B', '4 · B → A, return']);
    expect(rows[2]).toMatchObject({ mark: 'V ✕', tone: 'blocked', failing: ['video'] });
    expect(rows[3].isReturn).toBe(true);
    expect(ws.routeLengthText()).toBe('10.8 km + 5.3 km back');

    show({ ...m, returnToPilot: false }, null);
    expect(ws.legRows().some((r) => r.isReturn)).toBe(false);
    expect(ws.legRows()[0]).toMatchObject({ mark: '', tone: 'none' });
    expect(ws.routeLengthText()).toBe('10.8 km');
  });

  it('marks a warning leg with an exclamation mark', () => {
    const m = exampleMission();
    const r = routeResult(m, 'normal');
    show(m, { ...r, legs: r.legs!.map((l, i) => (i === 0 ? { ...l, status: 'warn' as const } : l)) });
    expect(ws.legRows()[0]).toMatchObject({ mark: '!', markLabel: 'Warning', tone: 'warn' });
  });

  it('says all is clear after the altitude fix, without the rf-core info lines', () => {
    const m = exampleMission();
    const fixed = { ...m, waypoints: m.waypoints.map((w, i) => (i === 2 ? { ...w, aglM: 345 } : w)) };
    const r = routeResult(fixed, 'normal');
    show(fixed, r);
    expect(r.legs![2].reasons![0].text).toContain('345 m AGL');
    expect(ws.verdict()).toBeNull();
    expect(ws.allClearText()).toBe('Control and video reach every leg of the route.');
    expect(fixAltitude(m).id).toBe('leg3-345m-agl');
  });

  it('falls back to generic all-clear texts', () => {
    const m = exampleMission();
    const r = routeResult(m, 'normal');
    const clear: RouteResult = { ...r, reasons: [], legs: r.legs!.map((l) => ({ ...l, status: 'ok', reasons: [], worst: { control: l.worst?.control } })) };
    show(m, clear);
    expect(ws.allClearText()).toBe('Control and video reach every leg of the route.');
    show({ ...m, evaluate: 'control' }, clear);
    expect(ws.allClearText()).toBe('Video is not checked in this mode; the control link reaches every leg.');
  });

  it('describes the battery plan before and after the calculation', () => {
    const m = exampleMission();
    show(m, null);
    expect(ws.batteryPlanText()).toMatch(/^6S 6000 mAh · ≈ \d+\.\d km$/);
    store.result.set(routeResult(m, 'normal'));
    expect(ws.batteryPlanText()).toBe('6S 6000 mAh · 10.2 km');
    show({ ...m, battery: 'unknown-pack' }, null);
    expect(ws.batteryPlanText()).toBe('unknown-pack · —');
    show({ ...m, battery: undefined }, null);
    expect(ws.batteryPlanText()).toBe('no battery · —');
  });

  it('names the forecast tier in the header', () => {
    const m = exampleMission();
    show(m, null);
    expect(ws.timeText()).toMatch(/^Now · \d\d:\d\d local · forecast hourly$/);
    store.result.set({ ...routeResult(m, 'normal'), forecastTier: 'long-range' });
    expect(ws.timeText()).toContain('long-range forecast');
    store.result.set({ ...routeResult(m, 'normal'), forecastTier: 'off' });
    expect(ws.timeText()).toContain('weather off · standard atmosphere');
    store.result.set(null);
    show({ ...m, time: { mode: 'planned', utc: '2100-01-01T12:00:00Z' } }, null);
    expect(ws.timeText()).toContain('beyond the forecast · standard atmosphere');
    expect(ws.timeText()).not.toContain('Now');
  });

  it('opens the why drawer on the failing link of a leg', () => {
    const m = exampleMission();
    show(m, routeResult(m, 'normal'));
    ws.openWhy(3);
    expect(ws.why()).toEqual({ leg: 3, link: 'video' });
    expect(ws.whyLeg()?.index).toBe(2);
    expect(ws.whyLink()?.reachable).toBe(false);
    ws.openWhy(1);
    expect(ws.why()).toEqual({ leg: 1, link: 'video' });
    ws.openWhy(1, 'control');
    expect(ws.whyLink()?.reachable).toBe(true);
    ws.openWhy(9);
    expect(ws.why()).toEqual({ leg: 1, link: 'control' });
    expect(ws.groundAt().pilot).toBeDefined();

    ws.resetToEmpty();
    expect(ws.why()).toBeNull();
    expect(ws.mission()).toBeNull();
    expect(ws.legRows()).toEqual([]);
    expect(ws.routeLengthText()).toBe('');
    expect(ws.batteryPlanText()).toBe('');
    expect(ws.timeText()).toBe('');
  });
});
