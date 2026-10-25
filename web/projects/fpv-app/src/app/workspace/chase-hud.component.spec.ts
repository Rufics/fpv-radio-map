import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore } from '@rfmap/ui-kit';
import { AltitudeView, FPV_ALTITUDE } from '../core/fpv-altitude';
import { ChaseView } from '../core/fpv-chase';
import { FpvDataService } from '../core/fpv-data.service';
import { fpvAltitudeMock } from '../mock/fpv-altitude-mock';
import { exampleMission, routeResult } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { ChaseHudComponent } from './chase-hud.component';

const mission = { ...exampleMission(), modules: { [FPV_ALTITUDE]: {} } };

describe('ChaseHudComponent', () => {
  let fixture: ComponentFixture<ChaseHudComponent>;
  let chase: ChaseView;
  const el = () => fixture.nativeElement as HTMLElement;
  const text = (sel: string) =>
    el().querySelector(sel)?.textContent?.replace(/\s+/g, ' ').trim() ?? '';

  beforeEach(async () => {
    MockApiHarness.setUp();
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
    const store = TestBed.inject(MissionStore);
    store.newMission(mission);
    store.result.set({
      ...routeResult(mission, 'normal'),
      modules: { [FPV_ALTITUDE]: fpvAltitudeMock(mission) },
    });
    chase = TestBed.inject(ChaseView);
    fixture = TestBed.createComponent(ChaseHudComponent);
  });

  afterEach(() => MockApiHarness.tearDown());

  it('shows nothing until the chase starts', () => {
    fixture.detectChanges();
    expect(el().querySelector('.read')).toBeNull();
  });

  it('reads out the heights, the climb, what is next and where it is on the timeline', () => {
    chase.start();
    chase.playing.set(false);
    chase.seek(5000);
    fixture.detectChanges();
    const f = chase.frame()!;
    expect(text('.big .v')).toBe(String(Math.round(f.aglM)));
    expect(text('.row')).toContain(`${Math.round(f.mslM)} m MSL`);
    expect(el().querySelectorAll('.ahead .a').length).toBe(chase.ahead().length);
    expect(text('.ahead .a')).toContain(`Next: ${chase.ahead()[0].text}`);
    expect(el().querySelectorAll('.legs .leg').length).toBe(chase.track()!.legs.length);
    const range = el().querySelector<HTMLInputElement>('input[type=range]')!;
    expect(Number(range.value)).toBe(5000);
    range.value = '2000';
    range.dispatchEvent(new Event('input'));
    expect(chase.s()).toBe(2000);
    expect(TestBed.inject(AltitudeView).cursorS()).toBe(2000);
  });

  it('plays, pauses, changes speed and exits', () => {
    chase.start();
    fixture.detectChanges();
    const play = el().querySelector<HTMLButtonElement>('.play')!;
    expect(play.getAttribute('aria-label')).toBe('Pause');
    play.click();
    fixture.detectChanges();
    expect(chase.playing()).toBe(false);
    expect(play.getAttribute('aria-label')).toBe('Play');
    const rates = el().querySelectorAll<HTMLButtonElement>('.rate');
    expect([...rates].map((b) => b.textContent?.trim())).toEqual(['×1', '×4', '×16']);
    rates[2].click();
    fixture.detectChanges();
    expect(chase.rate()).toBe(16);
    expect(rates[2].getAttribute('aria-checked')).toBe('true');
    [...el().querySelectorAll<HTMLButtonElement>('button')]
      .find((b) => b.textContent?.includes('Exit chase'))!
      .click();
    fixture.detectChanges();
    expect(chase.active()).toBe(false);
    expect(el().querySelector('.bar')).toBeNull();
  });

  it('marks a climb steeper than the drone can fly', () => {
    chase.start();
    chase.playing.set(false);
    const t = chase.track()!;
    const lim = chase.limits()!;
    // the first place the plan climbs faster than the drone's rate, if any
    const s = t.corridor.c.s.find((x) => t.frame(x).verticalMps > lim.climbMps + 0.05);
    if (s === undefined) return;
    chase.seek(s);
    fixture.detectChanges();
    expect(el().querySelector('.vs')?.classList).toContain('over');
    expect(text('.vs')).toMatch(/^Climb /);
  });
});
