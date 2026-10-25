import { Component, input } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MissionStore } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { exampleMission, routeResult } from '../mock/reference-case';
import { AltitudeView, FPV_ALTITUDE } from '../core/fpv-altitude';
import { FpvDataService } from '../core/fpv-data.service';
import { fpvAltitudeMock } from '../mock/fpv-altitude-mock';
import { MockApiHarness } from '../tests/mock-api-harness';
import { MapStageComponent } from './map-stage.component';
import { PhoneLayoutComponent } from './phone-layout.component';

/** The map itself is not under test here. */
@Component({ selector: 'fpv-map-stage', template: '' })
class MapStageStub {
  readonly fitPadding = input<unknown>();
  readonly compact = input(false);
  readonly interactiveEdit = input(true);
}

describe('PhoneLayoutComponent verdict', () => {
  beforeEach(() => {
    MockApiHarness.setUp();
    TestBed.overrideComponent(PhoneLayoutComponent, { set: { template: '', imports: [] } });
  });

  afterEach(() => MockApiHarness.tearDown());

  it('says whether the other link still works on the failing leg', () => {
    const verdict = (
      TestBed.createComponent(PhoneLayoutComponent).componentInstance as unknown as {
        verdictText: () => string | null;
      }
    ).verdictText;
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
      store.result.set({
        ...r,
        legs: r.legs!.map((l, i) =>
          i === 2
            ? {
                ...leg3,
                reasons: [],
                worst: {
                  video: { ...leg3.worst!.video!, reachable: video, marginDb: -1 },
                  control: { ...leg3.worst!.control!, reachable: control, marginDb: -2 },
                },
              }
            : l,
        ),
      });
    swap(true, false);
    ws.openWhy(3);
    expect(verdict()).toBe(
      'The control link is 2.00 dB short of the required level. Video still works.',
    );
    swap(false, false);
    expect(verdict()).toBe('The video link is 1.00 dB short of the required level.');
  });
});

describe('PhoneLayoutComponent sheet', () => {
  const mission = { ...exampleMission(), modules: { [FPV_ALTITUDE]: {} } };

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.overrideComponent(PhoneLayoutComponent, {
      remove: { imports: [MapStageComponent] },
      add: { imports: [MapStageStub] },
    });
    await MockApiHarness.settle(TestBed.inject(FpvDataService).load());
  });

  afterEach(() => MockApiHarness.tearDown());

  function render(withAltitude = true) {
    const store = TestBed.inject(MissionStore);
    const m = withAltitude ? mission : exampleMission();
    store.newMission(m);
    store.result.set({
      ...routeResult(m, 'normal'),
      ...(withAltitude ? { modules: { [FPV_ALTITUDE]: fpvAltitudeMock(m) } } : {}),
    });
    const fixture = TestBed.createComponent(PhoneLayoutComponent);
    fixture.detectChanges();
    const el = fixture.nativeElement as HTMLElement;
    const button = (text: string) =>
      [...el.querySelectorAll<HTMLButtonElement>('button')].find((b) =>
        b.textContent?.includes(text),
      );
    return { fixture, el, button, store };
  }

  it('keeps the plain sheet without the altitude result', () => {
    const { el } = render(false);
    expect(el.querySelector('fpv-phone-altitude')?.textContent?.trim()).toBe('');
    expect(el.querySelector('.handle-btn')).toBeNull();
    expect(el.querySelector('.chip.view')).toBeNull();
  });

  it('pulls the sheet up to the profile, by a tap or a drag on the handle', () => {
    const { fixture, el, button } = render();
    const handle = el.querySelector<HTMLButtonElement>('.handle-btn')!;
    expect(handle.getAttribute('aria-expanded')).toBe('false');
    expect(el.querySelector('fpv-altitude-drawer')).toBeNull();
    handle.click();
    fixture.detectChanges();
    expect(el.querySelector('.sheet')?.classList).toContain('up');
    expect(el.querySelector('fpv-altitude-drawer.compact')).not.toBeNull();
    // a drag down closes it, and the click after the drag does not open it again
    handle.dispatchEvent(new PointerEvent('pointerdown', { clientY: 100 }));
    handle.dispatchEvent(new PointerEvent('pointerup', { clientY: 200 }));
    handle.click();
    fixture.detectChanges();
    expect(handle.getAttribute('aria-expanded')).toBe('false');
    handle.dispatchEvent(new PointerEvent('pointerdown', { clientY: 300 }));
    handle.dispatchEvent(new PointerEvent('pointerup', { clientY: 200 }));
    handle.click();
    fixture.detectChanges();
    expect(handle.getAttribute('aria-expanded')).toBe('true');
    // and the summary's own button
    handle.click();
    fixture.detectChanges();
    button('Profile')!.click();
    fixture.detectChanges();
    expect(handle.getAttribute('aria-expanded')).toBe('true');
  });

  it('opens the climb options full screen, and goes back to the map for a previewed fix', () => {
    const { fixture, el, button, store } = render();
    button('Climb options')!.click();
    fixture.detectChanges();
    expect(el.querySelector('#phone-climb-title')?.textContent).toBe('Climb options');
    expect(el.querySelector('fpv-climb-assistant')).not.toBeNull();
    expect(el.querySelector('.sheet')).toBeNull();
    el.querySelector<HTMLButtonElement>('[aria-label="Back to mission"]')!.click();
    fixture.detectChanges();
    expect(el.querySelector('.sheet')).not.toBeNull();
    button('Climb options')!.click();
    fixture.detectChanges();
    el.querySelector<HTMLButtonElement>(
      'fpv-climb-assistant .rf-btn--primary:not([disabled])',
    )!.click();
    fixture.detectChanges();
    expect(store.preview()).not.toBeNull();
    expect(el.querySelector('fpv-climb-assistant')).toBeNull();
    expect(el.querySelector('fpv-preview-panel')).not.toBeNull();
  });

  it('switches the map between 2D and 3D from its chip', () => {
    const { fixture, el } = render();
    const view = TestBed.inject(AltitudeView);
    const chip = el.querySelector<HTMLButtonElement>('.chip.view')!;
    expect(chip.textContent?.trim()).toBe('3D');
    chip.click();
    fixture.detectChanges();
    expect(view.mode()).toBe('3d');
    expect(chip.getAttribute('aria-pressed')).toBe('true');
    expect(chip.textContent?.trim()).toBe('2D');
    chip.click();
    expect(view.mode()).toBe('2d');
  });
});
