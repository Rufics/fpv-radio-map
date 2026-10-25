import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore } from '@rfmap/ui-kit';
import { AltitudeView, FPV_ALTITUDE, FpvAltitudeData } from '../core/fpv-altitude';
import { ChaseView } from '../core/fpv-chase';
import { FpvDataService } from '../core/fpv-data.service';
import { fpvAltitudeMock } from '../mock/fpv-altitude-mock';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { PhoneAltitudeComponent } from './phone-altitude.component';

const mission = { ...exampleMission(), modules: { [FPV_ALTITUDE]: {} } };

describe('PhoneAltitudeComponent', () => {
  let fixture: ComponentFixture<PhoneAltitudeComponent>;
  let store: MissionStore;
  const el = () => fixture.nativeElement as HTMLElement;
  const buttons = () => [...el().querySelectorAll<HTMLButtonElement>('button')];
  const button = (text: string) => buttons().find((b) => b.textContent?.includes(text));
  const show = (data: FpvAltitudeData) => {
    store.result.set({ ...routeResult(mission, 'normal'), modules: { [FPV_ALTITUDE]: data } });
    fixture.detectChanges();
  };

  beforeEach(async () => {
    MockApiHarness.setUp();
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    store = TestBed.inject(MissionStore);
    store.newMission(mission);
    fixture = TestBed.createComponent(PhoneAltitudeComponent);
  });

  afterEach(() => MockApiHarness.tearDown());

  it('shows nothing without the altitude result', () => {
    fixture.detectChanges();
    expect(el().textContent?.trim()).toBe('');
  });

  it('puts the worst spot, the height it needs and the recommended fix in big numbers', () => {
    const data = fpvAltitudeMock(mission);
    show(data);
    const sum = data.summary!;
    const c = data.corridor!;
    const i = c.s.findIndex((s) => s >= sum.lowestMarginAtM);
    const nums = [...el().querySelectorAll('.num .v')].map((n) => n.textContent?.trim());
    expect(el().querySelector('.rf-eyebrow')?.textContent).toContain(
      `${(sum.lowestMarginAtM / 1000).toFixed(1)} km`,
    );
    expect(sum.lowestMarginM).toBeLessThan(0);
    expect(el().querySelector('.num.bad .v')?.textContent).toContain(
      `${Math.abs(Math.round(sum.lowestMarginM))} m`,
    );
    expect(nums[0]).toBe(el().querySelector('.num.bad .v')?.textContent?.trim());
    expect(Number.parseInt(nums[1]!, 10)).toBeCloseTo(c.floorMslM[i] - c.groundMslM[i], -1);
    const rec = data.climb!.strategies.find((s) => s.id === data.climb!.recommended)!;
    const fix = button('Preview fix')!;
    expect(fix.textContent).toContain(rec.title);
    fix.click();
    expect(store.preview()).not.toBeNull();
    expect(TestBed.inject(AltitudeView).ghost()).toBe(rec.id);
  });

  it('never offers a fix the drone cannot fly, nor one the plan does not need', () => {
    const data = fpvAltitudeMock(mission);
    const climb = data.climb!;
    show({
      ...data,
      climb: {
        ...climb,
        strategies: climb.strategies.map((s) => ({ ...s, withinClimbRate: false })),
      },
    });
    expect(button('Preview fix')).toBeUndefined();
    const cor = data.corridor!;
    // over the floor, but 30 m over the ceiling half way, and no flyable fix: never called safe
    const mid = Math.floor(cor.s.length / 2);
    show({
      ...data,
      corridor: {
        ...cor,
        floorMslM: cor.plannedMslM.map((p) => p - 20),
        ceilingMslM: cor.plannedMslM.map((p, i) => (i === mid ? p - 30 : p + 50)),
      },
      summary: { ...data.summary!, lowestMarginM: 20 },
      climb: {
        ...climb,
        strategies: climb.strategies.map((s) => ({ ...s, withinClimbRate: false })),
      },
    });
    expect(button('Preview fix')).toBeUndefined();
    expect(el().querySelector('.ok')).toBeNull();
    expect(el().querySelector('.warn')?.textContent).toContain('Up to 30 m over the ceiling');
    // inside the corridor everywhere: nothing to fix
    show({
      ...data,
      corridor: {
        ...cor,
        floorMslM: cor.plannedMslM.map((p) => p - 20),
        ceilingMslM: cor.plannedMslM.map((p) => p + 50),
      },
      summary: { ...data.summary!, lowestMarginM: 20 },
      climb: { ...climb, plan: { ...climb.plan, overCeilingM: 0 } },
    });
    expect(button('Preview fix')).toBeUndefined();
    expect(el().querySelector('.warn')).toBeNull();
    expect(el().querySelector('.ok')?.textContent).toContain('inside the safe corridor');
    // after an edit the old result is not called safe
    store.update((m) => ({ ...m, name: 'edited' }));
    fixture.detectChanges();
    expect(el().querySelector('.ok')).toBeNull();
    expect(el().querySelector('.warn')?.textContent).toContain('recalculate');
  });

  it('opens the profile, the climb options and the chase', () => {
    show(fpvAltitudeMock(mission));
    let profile = 0;
    let assistant = 0;
    fixture.componentInstance.profile.subscribe(() => profile++);
    fixture.componentInstance.assistant.subscribe(() => assistant++);
    button('Profile')!.click();
    button('Climb options')!.click();
    expect([profile, assistant]).toEqual([1, 1]);
    button('Chase in 3D')!.click();
    fixture.detectChanges();
    expect(TestBed.inject(ChaseView).active()).toBe(true);
    expect(button('Chase in 3D')).toBeUndefined();
  });
});
