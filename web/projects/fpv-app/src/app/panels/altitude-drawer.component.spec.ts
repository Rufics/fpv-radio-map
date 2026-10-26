import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore } from '@rfmap/ui-kit';
import { AltitudeView, FPV_ALTITUDE } from '../core/fpv-altitude';
import { FpvDataService } from '../core/fpv-data.service';
import { fpvAltitudeMock } from '../mock/fpv-altitude-mock';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { AltitudeDrawerComponent } from './altitude-drawer.component';

/** The altitude profile over the reference case, rendered. */
describe('AltitudeDrawerComponent', () => {
  let fixture: ComponentFixture<AltitudeDrawerComponent>;
  let store: MissionStore;
  let view: AltitudeView;
  const el = () => fixture.nativeElement as HTMLElement;
  const mission = { ...exampleMission(), modules: { [FPV_ALTITUDE]: {} } };

  beforeEach(async () => {
    MockApiHarness.setUp();
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    fixture = TestBed.createComponent(AltitudeDrawerComponent);
    store = TestBed.inject(MissionStore);
    view = TestBed.inject(AltitudeView);
  });

  afterEach(() => MockApiHarness.tearDown());

  it('offers to turn the corridor on for a mission without it', () => {
    store.newMission(exampleMission());
    fixture.detectChanges();
    expect(el().textContent).toContain('Turn on the altitude corridor');
    expect(el().querySelector('button.rf-btn--primary')?.textContent).toContain(
      'Turn on and calculate',
    );
  });

  it('asks for a calculation until there is a result', () => {
    store.newMission(mission);
    fixture.detectChanges();
    expect(el().textContent).toContain('Calculate the mission to see its altitude profile.');
  });

  it('draws the profile and reads out both heights under the cursor', () => {
    store.newMission(mission);
    store.result.set({
      ...routeResult(mission, 'normal'),
      modules: { [FPV_ALTITUDE]: fpvAltitudeMock(mission) },
    });
    fixture.detectChanges();
    expect(el().querySelector('svg path.ground')).not.toBeNull();
    expect(el().querySelectorAll('svg path.plan').length).toBeGreaterThan(0);
    expect(el().querySelector('.summary')?.textContent).toMatch(
      /^Highest ground \d+ m at km \d+\.\d\d\./,
    );
    expect([...el().querySelectorAll('.xaxis .wpl')].map((e) => e.textContent)).toEqual([
      '1',
      '2',
      'B',
    ]);

    view.cursorS.set(1000);
    fixture.detectChanges();
    const read = el().querySelector('dl')!.textContent!;
    expect(read).toContain('km 1.00');
    expect(read).toContain('Above launch');
    expect(read).toContain('Above ground');

    view.corridorShown.set(false);
    fixture.detectChanges();
    expect(el().querySelector('svg path.band')).toBeNull();
  });

  it('shows the module warning when it could not draw the corridor', () => {
    store.newMission(mission);
    store.result.set({
      ...routeResult(mission, 'normal'),
      reasons: [
        {
          module: FPV_ALTITUDE,
          severity: 'warn',
          text: 'Altitude corridor not drawn: no terrain data.',
        },
      ],
      modules: { [FPV_ALTITUDE]: {} },
    });
    fixture.detectChanges();
    expect(el().textContent).toContain('Altitude corridor not drawn: no terrain data.');
  });

  it('moves the cursor with the keyboard and keeps it after a touch', () => {
    store.newMission(mission);
    store.result.set({
      ...routeResult(mission, 'normal'),
      modules: { [FPV_ALTITUDE]: fpvAltitudeMock(mission) },
    });
    fixture.detectChanges();
    const svg = el().querySelector('svg')!;
    const total = Number(svg.getAttribute('aria-valuemax'));
    expect(svg.getAttribute('role')).toBe('slider');
    expect(svg.getAttribute('tabindex')).toBe('0');
    expect(svg.getAttribute('aria-valuetext')).toMatch(/^Highest ground/);
    const press = (key: string) => {
      const e = new KeyboardEvent('keydown', { key, cancelable: true });
      svg.dispatchEvent(e);
      fixture.detectChanges();
      return e;
    };

    expect(press('End').defaultPrevented).toBe(true);
    expect(view.cursorS()).toBe(total);
    press('ArrowRight');
    expect(view.cursorS()).toBe(total);
    press('Home');
    press('ArrowRight');
    expect(view.cursorS()).toBeCloseTo(total / 100, 6);
    press('PageUp');
    expect(view.cursorS()).toBeCloseTo(total * 0.11, 6);
    press('ArrowLeft');
    press('PageDown');
    expect(view.cursorS()).toBe(0);
    expect(press('a').defaultPrevented).toBe(false);
    expect(press('constructor').defaultPrevented).toBe(false);
    const back = new KeyboardEvent('keydown', { key: 'ArrowLeft', altKey: true, cancelable: true });
    svg.dispatchEvent(back);
    expect(back.defaultPrevented).toBe(false);
    expect(svg.getAttribute('aria-valuetext')).toMatch(/^km 0\.00: plan \d+ m MSL/);

    view.cursorS.set(1000);
    svg.dispatchEvent(Object.assign(new Event('pointerleave'), { pointerType: 'touch' }));
    expect(view.cursorS()).toBe(1000);
    svg.dispatchEvent(Object.assign(new Event('pointerleave'), { pointerType: 'mouse' }));
    expect(view.cursorS()).toBeNull();
    view.cursorS.set(1000);
    svg.dispatchEvent(new Event('blur'));
    expect(view.cursorS()).toBeNull();
  });

  it('fits the phone sheet: one column, no close button, the assistant left to its own screen', () => {
    store.newMission(mission);
    store.result.set({
      ...routeResult(mission, 'normal'),
      modules: { [FPV_ALTITUDE]: fpvAltitudeMock(mission) },
    });
    fixture.detectChanges();
    expect(el().querySelector('fpv-climb-assistant')).not.toBeNull();
    expect(el().querySelector('[aria-label="Close altitude profile"]')).not.toBeNull();
    fixture.componentRef.setInput('compact', true);
    fixture.detectChanges();
    expect(el().classList).toContain('compact');
    expect(el().querySelector('fpv-climb-assistant')).toBeNull();
    expect(el().querySelector('[aria-label="Close altitude profile"]')).toBeNull();
    expect(el().querySelector('.hint')?.textContent).toContain('Touch the profile');
  });
});
