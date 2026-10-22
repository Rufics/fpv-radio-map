import { signal, Type } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { FlightLogRow, FlightView, MissionStore, RfmapApiClient } from '@rfmap/ui-kit';
import { FlightReplay } from '../core/flight-replay.service';
import { MockFlights } from '../mock/mock-flights';
import { exampleMission, routeResult } from '../mock/reference-case';
import { PlanFactPanelComponent } from './plan-fact-panel.component';
import { ReplayDrawerComponent } from './replay-drawer.component';
import { ReplayPanelComponent } from './replay-panel.component';
import { ReplaySheetComponent } from './replay-sheet.component';

/** What the replay views show for the mock flight, with the templates left out. */
describe('replay views', () => {
  let replay: FlightReplay;
  let flight: FlightView;
  let rows: FlightLogRow[];
  const api = {
    listFlightLogs: vi.fn(async () => rows),
    getFlightLog: vi.fn(async (_m: string, id: string) => ({ ...flight, id })),
    deleteFlightLog: vi.fn(async () => undefined),
  };

  // The views' members are protected: the tests read them as a template would.
  function create<T>(type: Type<T>): Record<string, unknown> {
    return TestBed.createComponent(type).componentInstance as unknown as Record<string, unknown>;
  }
  const call = <R>(c: Record<string, unknown>, name: string, ...args: unknown[]) => (c[name] as (...a: unknown[]) => R)(...args);

  beforeEach(() => {
    const mission = exampleMission();
    flight = MockFlights.flight('LOG00007.BFL', mission);
    flight = { ...flight, plan: MockFlights.planFact(flight, mission, routeResult(mission, 'normal')) };
    rows = [MockFlights.row({ ...flight, id: 'f1' }), MockFlights.row({ ...MockFlights.headerOnly('btfl_016.bbl'), id: 'f2' })];
    TestBed.configureTestingModule({
      providers: [
        { provide: RfmapApiClient, useValue: api },
        { provide: MissionStore, useValue: { missionId: signal('m1'), canEdit: signal(true) } },
      ],
    });
    for (const type of [ReplayPanelComponent, ReplayDrawerComponent, ReplaySheetComponent, PlanFactPanelComponent]) {
      TestBed.overrideComponent(type as Type<unknown>, { set: { template: '', imports: [] } });
    }
    replay = TestBed.inject(FlightReplay);
  });

  afterEach(() => {
    replay.pause();
    vi.clearAllMocks();
  });

  describe('ReplayPanelComponent', () => {
    it('is empty without a flight', () => {
      const c = create(ReplayPanelComponent);
      expect(call(c, 'title')).toBe('');
      expect(call(c, 'firmware')).toBe('');
      expect(call(c, 'headline')).toBe('');
      expect(call(c, 'summary')).toEqual([]);
      expect(call(c, 'readout')).toEqual([]);
      expect(call(c, 'started')).toBe('start time not logged');
    });

    it('tells the flight on screen and opens the logbook', async () => {
      const c = create(ReplayPanelComponent);
      replay.show(flight);
      expect(call(c, 'title')).toBe(titleOf(flight));
      expect(call(c, 'firmware')).toMatch(/^Betaflight/);
      expect(call<string>(c, 'headline')).toContain(' km');
      expect(call<{ label: string }[]>(c, 'summary').map((s) => s.label)).toContain('Distance flown');
      expect(call<{ label: string }[]>(c, 'readout').map((s) => s.label)).toContain('Battery');
      expect(call(c, 'clock', 62)).toBe('01:02');

      call(c, 'setDetail', 'logbook');
      await vi.waitFor(() => expect(replay.logbook()).toHaveLength(2));
      expect(replay.detail()).toBe('logbook');
      expect(call<{ value: string }[]>(c, 'totals')[0].value).toBe('1');
      expect(call(c, 'line', rows[0])).toContain('·');
      expect(call(c, 'reason', rows[1])).toBe('No flight data, header only');
      expect(call(c, 'started', rows[1])).toMatch(/ UTC$/);

      call(c, 'openRow', rows[0]);
      await vi.waitFor(() => expect(api.getFlightLog).toHaveBeenCalledWith('m1', 'f1'));
      call(c, 'openRow', { ...rows[0], id: undefined });
      expect(api.getFlightLog).toHaveBeenCalledTimes(1);
    });
  });

  describe('ReplayDrawerComponent', () => {
    it('draws a chart row per channel the flight has, and off route against the plan', () => {
      const c = create(ReplayDrawerComponent);
      expect(call(c, 'rows')).toEqual([]);
      replay.show(flight);
      expect(call(c, 'end')).toBe(replay.timeline()?.end);
      const keys = () => call<{ key: string }[]>(c, 'rows').map((r) => r.key);
      expect(keys()).toEqual(['alt', 'speed', 'cell', 'link']);
      replay.mode.set('plan-fact');
      expect(keys()).toEqual(['alt', 'speed', 'cell', 'link', 'off']);
      const cell = call<{ key: string; guideLabel?: string }[]>(c, 'rows').find((r) => r.key === 'cell');
      expect(cell?.guideLabel).toBe('3.50 V');
      expect(call(c, 'chartsShown')).toBe(true);
      expect(call<{ label: string }[]>(c, 'bandKeys').length).toBeGreaterThan(0);
      expect(call<unknown[]>(c, 'ticks')).toHaveLength(flight.story.length);
      expect(call<{ label: string }[]>(c, 'colourOptions').map((o) => o.label)).toContain('Battery');
      expect(call(c, 'cursorX')).toBe(0);
    });

    it('seeks with the pointer and the keys, and plays from the cursor', () => {
      const c = create(ReplayDrawerComponent);
      replay.show(flight);
      const target = { getBoundingClientRect: () => ({ left: 100, width: 200 }) };
      call(c, 'seekAt', { currentTarget: target, clientX: 200 });
      expect(replay.t()).toBeCloseTo((replay.timeline()!.end - replay.timeline()!.start) / 2 + replay.timeline()!.start);
      call(c, 'drag', { currentTarget: target, clientX: 100, buttons: 0 });
      expect(replay.t()).toBeGreaterThan(0);
      call(c, 'drag', { currentTarget: target, clientX: 100, buttons: 1 });
      expect(replay.t()).toBe(replay.timeline()!.start);
      call(c, 'seekAt', { currentTarget: { getBoundingClientRect: () => ({ left: 0, width: 0 }) }, clientX: 5 });
      expect(replay.t()).toBe(replay.timeline()!.start);

      const key = (k: string, shiftKey = false) => {
        const e = { key: k, shiftKey, preventDefault: vi.fn() };
        call(c, 'key', e);
        return e.preventDefault.mock.calls.length;
      };
      expect(key('ArrowRight', true)).toBe(1);
      expect(replay.t()).toBe(replay.timeline()!.start + 10);
      key('ArrowLeft');
      expect(replay.t()).toBe(replay.timeline()!.start + 9);
      key(' ');
      expect(replay.playing()).toBe(true);
      expect(key('x')).toBe(0);
      call(c, 'playFrom');
      expect(replay.playing()).toBe(true);
      expect(call(c, 'clock', 5)).toBe('00:05');
    });
  });

  describe('ReplaySheetComponent', () => {
    it('shows four tiles and the warnings on a phone', () => {
      const c = create(ReplaySheetComponent);
      expect(call(c, 'readout')).toEqual([]);
      expect(call(c, 'title')).toBe('');
      replay.show(flight);
      expect(call<{ label: string }[]>(c, 'readout').map((t) => t.label)).toEqual(['From home', 'Ground speed', 'Altitude', 'Battery']);
      expect(call<{ severity: string }[]>(c, 'important').every((e) => e.severity !== 'info')).toBe(true);
      expect(call(c, 'started')).toMatch(/ UTC$/);
      expect(call(c, 'start')).toBe(0);
      expect(call(c, 'end')).toBe(replay.timeline()?.end);
      call(c, 'seek', { target: { value: '12' } });
      expect(replay.t()).toBe(12);
      expect(call(c, 'clock', 12)).toBe('00:12');
    });
  });

  describe('PlanFactPanelComponent', () => {
    it('writes plan against fact', () => {
      const c = create(PlanFactPanelComponent);
      expect(call(c, 'plan')).toBeNull();
      replay.show(flight);
      const plan = call<{ rows: { metric: string }[] }>(c, 'plan');
      expect(plan.rows.length).toBeGreaterThan(0);
      expect(call(c, 'value', { metric: 'distance', label: '', unit: '', plan: 1200, fact: 1500 }, 'plan')).toBe('1.20 km');
      expect(call(c, 'delta', { metric: 'distance', label: '', unit: '', plan: 1, fact: 1, deltaPct: 4 })).toBe('+4 %');
      expect(call(c, 'margin', null)).toBe('–');
      expect(call(c, 'margin', -2.04)).toBe('−2.0 dB');
      expect(call(c, 'distance', 640)).toBe('640 m');
      expect(call(c, 'round', 2.6)).toBe(3);
    });
  });
});

function titleOf(v: FlightView): string {
  return v.craft || v.fileName || 'Flight';
}
