import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { PhoneLayoutComponent } from './phone-layout.component';

describe('PhoneLayoutComponent verdict', () => {
  beforeEach(() => {
    MockApiHarness.setUp();
    TestBed.overrideComponent(PhoneLayoutComponent, { set: { template: '', imports: [] } });
  });

  afterEach(() => MockApiHarness.tearDown());

  it('says whether the other link still works on the failing leg', () => {
    const verdict = (TestBed.createComponent(PhoneLayoutComponent).componentInstance as unknown as { verdictText: () => string | null }).verdictText;
    const store = TestBed.inject(MissionStore);
    const ws = TestBed.inject(FpvWorkspace);
    const m = exampleMission();
    const r = routeResult(m, 'normal');
    store.newMission(m);
    store.result.set(r);
    expect(verdict()).toBeNull();
    ws.openWhy(3);
    expect(verdict()).toBe('Ridge 622 m blocks the video link by 100 m at B. Control still works.');
    ws.openWhy(1);
    expect(verdict()).toBeNull();

    const leg3 = r.legs![2];
    const swap = (video: boolean, control: boolean) =>
      store.result.set({ ...r, legs: r.legs!.map((l, i) => (i === 2 ? { ...leg3, reasons: [], worst: { video: { ...leg3.worst!.video!, reachable: video, marginDb: -1 }, control: { ...leg3.worst!.control!, reachable: control, marginDb: -2 } } } : l)) });
    swap(true, false);
    ws.openWhy(3);
    expect(verdict()).toBe('The control link is 2.00 dB short of the required level. Video still works.');
    swap(false, false);
    expect(verdict()).toBe('The video link is 1.00 dB short of the required level.');
  });
});
