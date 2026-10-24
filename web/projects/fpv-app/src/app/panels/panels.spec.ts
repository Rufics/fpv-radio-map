import { ComponentFixture, TestBed } from '@angular/core/testing';
import { Type } from '@angular/core';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MissionStore, provideMapView } from '@rfmap/ui-kit';
import { FPV_CORE } from '../core/fpv-core';
import { FpvDataService } from '../core/fpv-data.service';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { ComparePanelComponent } from './compare-panel.component';
import { MissionTimeComponent } from './mission-time.component';
import { PlanPanelComponent } from './plan-panel.component';
import { StateCardsComponent } from './state-cards.component';

/** Small view-model rules of the side panels, with the dev mock backend behind them (templates not rendered). */
describe('side panels', () => {
  let store: MissionStore;
  let data: FpvDataService;

  function vmOf<T>(component: Type<unknown>): T {
    const fixture: ComponentFixture<unknown> = TestBed.createComponent(component);
    return fixture.componentInstance as T;
  }

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    for (const c of [ComparePanelComponent, MissionTimeComponent, PlanPanelComponent, StateCardsComponent]) TestBed.overrideComponent(c, { set: { template: '', imports: [] } });
    store = TestBed.inject(MissionStore);
    data = TestBed.inject(FpvDataService);
    await MockApiHarness.settle(data.load());
    store.newMission(exampleMission());
  });

  afterEach(() => MockApiHarness.tearDown());

  it('compare panel: marks B better, worse or unchanged per link', () => {
    const panel = vmOf<{ rows: () => { kind: string; d: number; mark: string }[] }>(ComparePanelComponent);
    const m = exampleMission();
    store.result.set({
      ...routeResult(m, 'normal'),
      modules: {
        [FPV_CORE]: {
          compare: {
            links: {
              control: { a: { worstMarginDb: 6 }, b: { worstMarginDb: 6.001 } },
              video: { a: { worstMarginDb: -3.74 }, b: { worstMarginDb: -0.74 }, deltaWorstDb: 3 },
            },
          },
        },
      },
    });
    expect(panel.rows().map((r) => r.mark)).toEqual(['=', '▲']);
    store.result.set({ ...routeResult(m, 'normal'), modules: { [FPV_CORE]: { compare: { links: { video: { deltaWorstDb: -2 }, control: { a: { worstMarginDb: 1 } } } } } } });
    expect(panel.rows().map((r) => [r.kind, r.mark])).toEqual([
      ['video', '▼'],
      ['control', '▼'],
    ]);
    expect(Number.isNaN(panel.rows()[1].d)).toBe(true);
  });

  it('mission time: says how far the planned day is and names the tier', () => {
    const t = vmOf<{ relative: () => string; tierText: (x: string) => string }>(MissionTimeComponent);
    const at = (days: number) => store.update((m) => ({ ...m, time: { mode: 'planned', utc: new Date(Date.now() + days * 86400e3).toISOString() } }));
    expect(t.relative()).toBe('today');
    at(1);
    expect(t.relative()).toBe('tomorrow');
    at(5);
    expect(t.relative()).toBe('in 5 days');
    at(-3);
    expect(t.relative()).toBe('3 days ago');
    expect(t.tierText('hourly')).toBe('hourly forecast');
    expect(t.tierText('long-range')).toBe('long-range forecast');
    expect(t.tierText('none')).toBe('no forecast');
  });

  it('plan panel: names each radio and whether the library entry is verified', () => {
    const plan = vmOf<{ radios: () => { kind: string; letter: string; name: string; source: string }[] }>(PlanPanelComponent);
    expect(plan.radios()).toEqual([
      { kind: 'control', letter: 'C', name: 'ELRS 433 MHz', source: 'library' },
      { kind: 'video', letter: 'V', name: 'digital 915 MHz', source: 'library' },
    ]);
    data.radios.update((list) => list.map((r) => (r.id === 'elrs-433-50hz' ? { ...r, verified: false } : r)));
    store.update((m) => ({ ...m, links: { ...m.links, video: { ...m.links.video!, radio: 'custom-radio' } } }));
    expect(plan.radios().map((r) => [r.name, r.source])).toEqual([
      ['ELRS 433 MHz', 'library · unverified'],
      ['custom-radio', 'pick one'],
    ]);
  });

  it('plan panel: an autonomous route may check no radio link at all', () => {
    const plan = vmOf<{
      radios: () => unknown[];
      setMode: (m: string) => void;
      setFlight: (m: string) => void;
      flightHint: (m: unknown) => string;
    }>(PlanPanelComponent);
    expect(plan.flightHint(store.mission())).toContain('control link');
    plan.setFlight('autonomous');
    plan.setMode('none');
    expect(store.mission()?.flightMode).toBe('autonomous');
    expect(plan.radios()).toEqual([]);
    expect(plan.flightHint(store.mission())).toContain('autopilot');
  });

  it('plan panel: a fixed-wing drone opts the mission into the envelope check', () => {
    const plan = vmOf<{ setDrone: (id: string) => void; droneHint: (m: unknown) => string | null }>(PlanPanelComponent);
    expect(plan.droneHint(store.mission())).toBeNull();
    plan.setDrone('fw-1800-mapper');
    expect(store.mission()?.drone).toBe('fw-1800-mapper');
    expect(store.mission()?.modules?.['fpv.airframe']).toEqual({});
    expect(plan.droneHint(store.mission())).toContain('checked against these limits');
    expect(plan.droneHint({ ...store.mission()!, modules: {} })).toContain('check is off');
    plan.setDrone('fpv-10in-recon');
    expect(store.mission()?.modules?.['fpv.airframe']).toBeUndefined();
  });

  it('plan panel: "Move on map" puts the map into pilot placing mode', () => {
    const plan = vmOf<{ movePilot: () => void; ws: { placing: () => string | null; why: () => unknown } }>(PlanPanelComponent);
    plan.movePilot();
    expect(plan.ws.placing()).toBe('pilot');
    expect(plan.ws.why()).toBeNull();
  });

  describe('state cards', () => {
    let written: string[];

    beforeEach(() => {
      written = [];
      const writeText = (t: string): Promise<void> => {
        written.push(t);
        return Promise.resolve();
      };
      Object.defineProperty(navigator, 'clipboard', { configurable: true, value: { writeText } });
    });

    afterEach(() => {
      Reflect.deleteProperty(navigator, 'clipboard');
    });

    it('asks the admin for the missing area with its bbox and source', async () => {
      const cards = vmOf<{ askAdmin: (leg: number) => Promise<void> }>(StateCardsComponent);
      const m = exampleMission();
      const r = routeResult(m, 'missing-elevation');
      store.result.set({ ...r, legs: r.legs!.map((l) => (l.missingData ? { ...l, missingData: { ...l.missingData, source: 'copernicus-glo30' } } : l)) });
      await cards.askAdmin(3);
      expect(written[0]).toMatch(/^Please add elevation data for mission "North ridge recon", leg 3: \d+\.\d{5}–\d+\.\d{5} N, \d+\.\d{5}–\d+\.\d{5} E \(copernicus-glo30\)$/);

      store.result.set({ ...r, legs: r.legs!.map((l) => (l.missingData ? { ...l, missingData: { ...l.missingData, source: undefined } } : l)) });
      await cards.askAdmin(3);
      expect(written[1]).toMatch(/ E$/);
      expect(written[1]).not.toContain('(');

      store.result.set(null);
      await cards.askAdmin(3);
      expect(written[2]).toMatch(/N, .* E \(route extent\)$/);
      vi.runOnlyPendingTimers();
    });

    it('recalculates without the weather module', async () => {
      const cards = vmOf<{ withoutWeather: () => Promise<void> }>(StateCardsComponent);
      await MockApiHarness.settle(cards.withoutWeather());
      await MockApiHarness.wait(3_000);
      const mods = store.mission()!.modules!;
      expect(mods['rf.weather']).toBeUndefined();
      expect(mods['fpv.battery']).toEqual({});
      expect(store.result()?.forecastTier).toBe('off');
    });
  });
});
