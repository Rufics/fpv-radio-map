import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore } from '@rfmap/ui-kit';
import { AltitudeView, FPV_ALTITUDE, FpvAltitudeData, FpvClimbData } from '../core/fpv-altitude';
import { FpvDataService } from '../core/fpv-data.service';
import { fpvAltitudeMock } from '../mock/fpv-altitude-mock';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { AltitudeDrawerComponent } from './altitude-drawer.component';
import { ClimbAssistantComponent } from './climb-assistant.component';

/** The climb assistant beside the altitude profile, over the reference case. */
describe('ClimbAssistantComponent', () => {
  let store: MissionStore;
  let view: AltitudeView;
  const mission = { ...exampleMission(), modules: { [FPV_ALTITUDE]: {} } };

  const show = (data: FpvAltitudeData) => {
    store.newMission(mission);
    store.result.set({ ...routeResult(mission, 'normal'), modules: { [FPV_ALTITUDE]: data } });
  };

  beforeEach(async () => {
    MockApiHarness.setUp();
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    store = TestBed.inject(MissionStore);
    view = TestBed.inject(AltitudeView);
  });

  afterEach(() => MockApiHarness.tearDown());

  describe('in the profile drawer', () => {
    let fixture: ComponentFixture<AltitudeDrawerComponent>;
    const el = () => fixture.nativeElement as HTMLElement;

    beforeEach(() => {
      fixture = TestBed.createComponent(AltitudeDrawerComponent);
    });

    it('lists the strategies with the energy-optimal one recommended', () => {
      show(fpvAltitudeMock(mission));
      fixture.detectChanges();
      const cards = [...el().querySelectorAll('fpv-climb-assistant .card')];
      expect(cards.map((c) => c.querySelector('h4')?.textContent)).toEqual([
        'Energy-optimal climb',
        'Climb over launch',
        'Gradual up and down',
      ]);
      expect(cards[0].querySelector('.badge')?.textContent).toBe('Recommended');
      expect(cards[0].querySelector('.deltas dt')?.textContent).toBe('Energy');
      expect(cards[0].querySelector('.deltas dd')?.textContent).toMatch(/^−\d+\.\d Wh$/);
      expect(cards[0].querySelector('.deltas dd')?.classList).toContain('is-better');
    });

    it('draws a strategy over the profile and reads it out under the cursor', () => {
      show(fpvAltitudeMock(mission));
      fixture.detectChanges();
      expect(el().querySelector('svg path.ghost')).toBeNull();
      const toggle = el().querySelector<HTMLButtonElement>('.card .actions button')!;
      toggle.click();
      fixture.detectChanges();
      expect(view.ghost()).toBe('energy');
      expect(el().querySelector('svg path.ghost')?.getAttribute('d')).toMatch(/^M0\.0,/);
      expect(toggle.getAttribute('aria-pressed')).toBe('true');
      expect(toggle.textContent?.trim()).toBe('Hide from profile');
      expect(el().querySelector('.keys')?.textContent).toContain('Energy-optimal climb');
      view.cursorS.set(1000);
      fixture.detectChanges();
      expect(el().querySelector('dl')?.textContent).toContain('Energy-optimal climb');
      toggle.click();
      fixture.detectChanges();
      expect(view.ghost()).toBeNull();
      expect(el().querySelector('svg path.ghost')).toBeNull();
    });

    it('opens the fix preview, and only on a current result', () => {
      show(fpvAltitudeMock(mission));
      fixture.detectChanges();
      const preview = () => el().querySelectorAll<HTMLButtonElement>('.card .actions button')[3];
      expect(preview().textContent?.trim()).toBe('Preview fix');
      preview().click();
      expect(store.preview()?.fix.id).toBe('fpv.altitude/climb-over-launch');
      expect(view.ghost()).toBe('over-launch');
      store.cancelPreview();
      store.resultStale.set(true);
      fixture.detectChanges();
      expect(preview().disabled).toBe(true);
      expect(preview().title).toContain('Recalculate first');
    });

    it('hides the assistant without its data, and the fix from a viewer', () => {
      const { climb: _climb, ...bare } = fpvAltitudeMock(mission);
      show(bare);
      fixture.detectChanges();
      expect(el().querySelector('fpv-climb-assistant')).toBeNull();
      expect(el().querySelector('.body.has-climb')).toBeNull();
      show(fpvAltitudeMock(mission));
      store.role.set('view');
      fixture.detectChanges();
      expect(el().querySelectorAll('.card .actions button')).toHaveLength(3);
    });
  });

  describe('on its own', () => {
    let fixture: ComponentFixture<ClimbAssistantComponent>;
    const el = () => fixture.nativeElement as HTMLElement;
    const stats = {
      riseM: 200,
      timeS: 600,
      minClearanceM: 5,
      minClearanceAtM: 0,
      overCeilingM: 0,
      steepestClimbMps: 2,
      steepestClimbAtM: 0,
      steepestDescentMps: 3,
      waypoints: 3,
    };
    const climb: FpvClimbData = {
      recommended: 'energy',
      climbRateMps: 2,
      descentRateMps: 4,
      airspeedMps: 15,
      energyEstimated: false,
      heightStepM: 5,
      fitToleranceM: 5,
      plan: stats,
      notes: ['No drone picked.'],
      strategies: [
        {
          ...stats,
          id: 'energy',
          title: 'Energy-optimal climb',
          summary: 'Least climb.',
          withinClimbRate: false,
          steepestClimbMps: 4.2,
          addedWaypoints: 1,
          waypoints: 4,
          mslM: [],
          fix: { id: 'e', patch: {} },
        },
        {
          ...stats,
          id: 'over-launch',
          title: 'Climb over launch',
          summary: 'Level.',
          withinClimbRate: true,
          riseM: 260,
          timeS: 590,
          minClearanceM: -3,
          overCeilingM: 12,
          addedWaypoints: 0,
          mslM: [],
          fix: { id: 'o', patch: {} },
        },
        {
          ...stats,
          id: 'gradual',
          title: 'Gradual up and down',
          summary: 'Steady.',
          withinClimbRate: false,
          steepestDescentMps: 5.5,
          addedWaypoints: 0,
          mslM: [],
          fix: { id: 'g', patch: {} },
        },
      ],
    };

    it('compares the total climb without a drone, and warns about the rate and the ceiling', () => {
      show({ climb });
      fixture = TestBed.createComponent(ClimbAssistantComponent);
      fixture.detectChanges();
      const cards = [...el().querySelectorAll('.card')];
      const dd = (c: Element) =>
        [...c.querySelectorAll('.deltas dd')].map((d) => [d.textContent, d.className]);
      expect(el().querySelector('.lead')?.textContent).toContain('total climb');
      expect(dd(cards[0])).toEqual([
        ['0 m', 'rf-mono is-same'],
        ['0:00', 'rf-mono is-same'],
        ['+5 m', 'rf-mono is-same'],
      ]);
      expect(dd(cards[1])).toEqual([
        ['+60 m', 'rf-mono is-worse'],
        ['−0:10', 'rf-mono is-better'],
        ['−3 m', 'rf-mono is-worse'],
      ]);
      expect(cards[0].querySelector('.warn')?.textContent).toBe(
        "Climbs at up to 4.2 m/s: faster than the drone's 2.0 m/s.",
      );
      expect(cards[0].querySelector('.wps')?.textContent).toBe(
        '4 waypoints, 1 added to follow the profile',
      );
      expect(cards[1].querySelector('.warn')?.textContent).toBe('Up to 12 m over the ceiling.');
      expect(cards[1].querySelector('.wps')?.textContent).toBe('3 waypoints, none added');
      expect(cards[2].querySelector('.warn')?.textContent).toBe(
        "Descends at up to 5.5 m/s: faster than the drone's 4.0 m/s.",
      );
      expect(el().querySelector('.notes')?.textContent).toContain('No drone picked.');
    });

    it('offers no fix the drone cannot fly', () => {
      show({ climb });
      fixture = TestBed.createComponent(ClimbAssistantComponent);
      fixture.detectChanges();
      const previews = [
        ...el().querySelectorAll<HTMLButtonElement>('.card .actions .rf-btn--primary'),
      ];
      expect(previews.map((b) => b.disabled)).toEqual([true, false, true]);
      expect(previews[0].title).toBe(
        'The drone cannot fly this line at its climb and descent rates',
      );
    });

    it('says nothing without the assistant', () => {
      show({});
      fixture = TestBed.createComponent(ClimbAssistantComponent);
      fixture.detectChanges();
      expect(el().textContent?.trim()).toBe('');
    });
  });
});
