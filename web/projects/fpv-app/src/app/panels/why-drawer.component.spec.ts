import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore, RouteResult } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { WhyDrawerComponent } from './why-drawer.component';

interface WhyVm {
  title: string;
  subtitle: string;
  links: { kind: string; label: string }[];
  terms: { id: string; label: string }[];
  fix?: { id?: string };
  fixTitle: string;
}

/** The "why" drawer view model over the reference case (the charts are not rendered). */
describe('WhyDrawerComponent', () => {
  let fixture: ComponentFixture<WhyDrawerComponent>;
  let store: MissionStore;
  let ws: FpvWorkspace;
  const vm = () => (fixture.componentInstance as unknown as { vm: () => WhyVm | null }).vm();
  const mission = exampleMission();

  beforeEach(() => {
    MockApiHarness.setUp();
    TestBed.overrideComponent(WhyDrawerComponent, { set: { template: '', imports: [] } });
    fixture = TestBed.createComponent(WhyDrawerComponent);
    store = TestBed.inject(MissionStore);
    ws = TestBed.inject(FpvWorkspace);
    store.newMission(mission);
  });

  afterEach(() => MockApiHarness.tearDown());

  it('is empty until a leg is picked', () => {
    store.result.set(routeResult(mission, 'normal'));
    expect(vm()).toBeNull();
  });

  it('explains the blocked video link of leg 3 at B with readable term names', () => {
    store.result.set(routeResult(mission, 'normal'));
    ws.openWhy(3);
    const v = vm()!;
    expect(v.title).toMatch(/^Why leg 3 fails · A → drone at B · video \d+ MHz$/);
    expect(v.links).toEqual([
      { kind: 'control', label: 'C ✓' },
      { kind: 'video', label: 'V ✕' },
    ]);
    const labels = v.terms.map((t) => t.label);
    expect(labels).toContain('Free-space loss');
    expect(labels).toContain('Ridge diffraction');
    expect(v.fix?.id).toBe('leg3-345m-agl');
    expect(v.fixTitle).toMatch(/^[a-z0-9]/);
  });

  it('places the worst sample at the start of the return leg', () => {
    store.result.set(routeResult(mission, 'normal'));
    ws.openWhy(4, 'control');
    expect(vm()!.title).toMatch(/^Leg 4 · reachable · A → drone at B · control/);
  });

  it('places a worst sample inside the leg by its distance', () => {
    const r = routeResult(mission, 'normal');
    const leg0 = r.legs![0];
    const control = { ...leg0.worst!.control!, sample: { ...leg0.worst!.control!.sample, alongLegM: 1500 } };
    const moved: RouteResult = { ...r, legs: [{ ...leg0, worst: { ...leg0.worst, control } }, ...r.legs!.slice(1)] };
    store.result.set(moved);
    ws.openWhy(1, 'control');
    expect(vm()!.subtitle).toMatch(/^A → drone 1\.5 km into the leg · control/);
  });
});
