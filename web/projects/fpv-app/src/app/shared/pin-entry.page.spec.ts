import { ComponentFixture, TestBed } from '@angular/core/testing';
import { provideRouter } from '@angular/router';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { RfmapApiClient } from '@rfmap/ui-kit';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { PinEntryPage } from './pin-entry.page';

/** Opening a shared link with a PIN against the dev mock backend. */
describe('PinEntryPage', () => {
  let fixture: ComponentFixture<PinEntryPage>;
  const vm = () => fixture.componentInstance as unknown as { press(k: string): void; open(): Promise<void>; error(): string | null; state(): string };

  async function enter(pin: string): Promise<void> {
    for (const k of pin) vm().press(k);
    await MockApiHarness.settle(vm().open());
    fixture.detectChanges();
  }

  beforeEach(async () => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideRouter([])] });
    const api = TestBed.inject(RfmapApiClient);
    const created = await MockApiHarness.settle(api.createMission(exampleMission()));
    const link = await MockApiHarness.settle(api.createLink(created.missionId, { role: 'view', pin: '123456' }, created.ownerKey));
    fixture = TestBed.createComponent(PinEntryPage);
    fixture.componentRef.setInput('token', (link as unknown as { token: string }).token);
    fixture.detectChanges();
    await MockApiHarness.wait(500);
    fixture.detectChanges();
  });

  afterEach(() => {
    fixture.destroy();
    MockApiHarness.tearDown();
  });

  it('counts down the tries left after wrong PINs', async () => {
    expect(vm().state()).toBe('entry');
    await enter('000000');
    expect(vm().error()).toBe('Wrong PIN. 4 tries left before the link pauses.');
    expect(fixture.nativeElement.textContent).toContain('4 tries left');
    await enter('000000');
    await enter('000000');
    await enter('000000');
    expect(vm().error()).toBe('Wrong PIN. 1 try left before the link pauses.');
    await enter('000000');
    expect(vm().state()).toBe('paused');
  });
});
