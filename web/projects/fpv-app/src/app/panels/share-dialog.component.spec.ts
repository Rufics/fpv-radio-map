import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { CredentialStore, MissionExporter, MissionStore, RfmapApiClient } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { ShareDialogComponent } from './share-dialog.component';

/** The share dialog against the dev mock backend's link endpoints. */
describe('ShareDialogComponent', () => {
  let fixture: ComponentFixture<ShareDialogComponent>;
  let creds: CredentialStore;
  let api: RfmapApiClient;
  const settle = MockApiHarness.settle;

  const el = () => fixture.nativeElement as HTMLElement;
  const cards = () => [...el().querySelectorAll<HTMLElement>('.card')];
  const button = (root: HTMLElement, text: string) => [...root.querySelectorAll('button')].find((b) => b.textContent?.trim() === text)!;

  async function open(missionId: string, ownerKey: string | null): Promise<void> {
    TestBed.inject(MissionStore).loadFromServer(missionId, { mission: exampleMission(), role: 'edit', revision: 1 });
    if (ownerKey) creds.setOwnerKey(missionId, ownerKey);
    fixture = TestBed.createComponent(ShareDialogComponent);
    fixture.detectChanges();
    await MockApiHarness.wait(500);
    fixture.detectChanges();
  }

  async function newMission(): Promise<{ missionId: string; ownerKey: string }> {
    const created = await settle(api.createMission(exampleMission()));
    creds.setSession(created.missionId, created.session);
    return { missionId: created.missionId, ownerKey: created.ownerKey };
  }

  beforeEach(() => {
    MockApiHarness.setUp();
    creds = TestBed.inject(CredentialStore);
    api = TestBed.inject(RfmapApiClient);
  });

  afterEach(() => {
    fixture?.destroy();
    MockApiHarness.tearDown();
  });

  it('shows the newest active link of each role with its opens and expiry', async () => {
    await open('m-north-ridge', 'owner-demo-key');
    const [edit, view] = cards();
    expect(edit.textContent).toContain('Can edit');
    expect(edit.querySelector('.status')?.textContent).toMatch(/^opened 2 times · expires /);
    expect(edit.textContent).toContain('A edit link exists (Co-pilot)');
    // the expired and revoked view links are skipped
    expect(view.querySelector('.status')?.textContent).toBe('opened 7 times · no expiry');
    expect(view.textContent).toContain('(Phone crew)');
    expect(view.querySelector('.url')).toBeNull();
  });

  it('asks for the owner key before it lists links', async () => {
    const { missionId } = await newMission();
    await open(missionId, null);
    expect(el().textContent).toContain('This browser does not have it');
    expect(cards()[0].querySelector('.status')).toBeNull();
    expect(button(cards()[0], 'Create link').disabled).toBe(true);
  });

  it('refuses a short PIN and creates a link with a 6-digit one', async () => {
    const { missionId, ownerKey } = await newMission();
    await open(missionId, ownerKey);
    const view = cards()[1];
    button(view, 'Create link').click();
    fixture.detectChanges();
    expect(cards()[1].querySelector('.rf-field-error')?.textContent).toBe('The PIN must be exactly 6 digits.');

    const pin = cards()[1].querySelector<HTMLInputElement>('input.pin')!;
    pin.value = '12a3456';
    pin.dispatchEvent(new Event('input'));
    fixture.detectChanges();
    expect(cards()[1].querySelector('.rf-field-error')).toBeNull();
    button(cards()[1], 'Create link').click();
    await MockApiHarness.wait(1_000);
    fixture.detectChanges();

    const card = cards()[1];
    expect(card.querySelector('.url')?.textContent).toMatch(new RegExp(`^${location.origin}/m/v/`));
    expect(card.querySelector('.status')?.textContent).toBe('opened 0 times · no expiry');
    expect(card.textContent).toContain('PIN required · opens on phone');

    button(card, 'New link').click();
    fixture.detectChanges();
    expect(cards()[1].querySelector('.url')).toBeNull();
    expect(cards()[1].querySelector('input.pin')).not.toBeNull();
  });

  it('shows a link paused after wrong PINs, and when it was last opened', async () => {
    const { missionId, ownerKey } = await newMission();
    const link = await settle(api.createLink(missionId, { role: 'edit', pin: '123456', label: 'Crew', expiresAt: new Date(Date.now() + 86400e3).toISOString() }, ownerKey));
    const token = (link as unknown as { token: string }).token;
    await settle(api.openLink(token, '123456'));
    for (let i = 0; i < 5; i++) await settle(api.openLink(token, '000000')).catch(() => undefined);

    await open(missionId, ownerKey);
    const edit = cards()[0];
    const status = edit.querySelector('.status')!;
    expect(status.classList.contains('paused')).toBe(true);
    expect(status.textContent).toMatch(/^opened 1 time · expires .* · paused after wrong PINs until /);
    expect(edit.textContent).toContain('last opened');
  });

  it('revokes a link so the card offers a new one', async () => {
    const { missionId, ownerKey } = await newMission();
    await settle(api.createLink(missionId, { role: 'view', pin: '123456', label: 'Crew' }, ownerKey));
    await open(missionId, ownerKey);
    expect(cards()[1].textContent).toContain('(Crew)');
    button(cards()[1], 'Revoke').click();
    await MockApiHarness.wait(1_000);
    fixture.detectChanges();
    expect(cards()[1].querySelector('input.pin')).not.toBeNull();
    expect(cards()[1].querySelector('.status')).toBeNull();
  });

  it('exports KML through fpv.core with the chosen heights', async () => {
    const download = vi.spyOn(MissionExporter, 'download').mockImplementation(() => undefined);
    await settle(TestBed.inject(FpvDataService).load());
    const { missionId, ownerKey } = await newMission();
    await open(missionId, ownerKey);
    const alt = el().querySelector<HTMLSelectElement>('#kml-alt')!;
    alt.value = 'absolute';
    alt.dispatchEvent(new Event('change'));
    button(el(), 'KML').click();
    await MockApiHarness.wait(3_000);
    fixture.detectChanges();
    expect(el().querySelector('[role=status]')?.textContent).toBe('KML exported by fpv.core, heights above sea level.');
    expect(download).toHaveBeenCalledTimes(1);
    expect(download.mock.calls[0][0].content).toContain('<kml');
    download.mockRestore();
  });
});
