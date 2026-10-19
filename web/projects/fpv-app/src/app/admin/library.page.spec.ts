import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { ApiError, CredentialStore, LibraryKind } from '@rfmap/ui-kit';
import { LIBRARY } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { LibraryPage } from './library.page';

type Entry = Record<string, unknown>;

interface LibraryVm {
  entries(): Entry[];
  selectedId: { (): string | null; set(v: string | null): void };
  text: { (): string; set(v: string): void };
  message(): { ok: boolean; text: string } | null;
  serverIssues(): { path: string; message: string }[];
  conflict(): boolean;
  validation(): { ok: boolean };
  summary(e: Entry): string;
  selectKind(k: LibraryKind): Promise<void>;
  select(e: Entry): void;
  newEntry(): void;
  save(): Promise<void>;
  loadCurrent(): Promise<void>;
}

/** The library editor saving through the dev mock backend (versions, 409, 422, admin token). */
describe('LibraryPage', () => {
  let fixture: ComponentFixture<LibraryPage>;
  let page: LibraryVm;
  let creds: CredentialStore;

  const seeded = { ...LIBRARY.batteries[1], version: 1, verified: false };

  function draft(e: Entry): void {
    page.text.set(JSON.stringify(e));
  }

  async function save(): Promise<void> {
    await MockApiHarness.settle(page.save());
    fixture.detectChanges();
  }

  beforeEach(async () => {
    MockApiHarness.setUp();
    creds = TestBed.inject(CredentialStore);
    fixture = TestBed.createComponent(LibraryPage);
    page = fixture.componentInstance as unknown as LibraryVm;
    fixture.detectChanges();
    await MockApiHarness.wait(500);
    fixture.detectChanges();
  });

  afterEach(() => {
    creds.setAdminToken(null);
    fixture.destroy();
    MockApiHarness.tearDown();
  });

  it('loads the radio profiles and summarises entries of every kind', () => {
    expect(page.entries()).toHaveLength(LIBRARY.radios.length);
    expect(page.selectedId()).toBe(LIBRARY.radios[0].id);
    expect(page.summary({ frequencyHz: 915e6 })).toBe('915 MHz');
    expect(page.summary({ gainDbi: 5 })).toBe('5 dBi');
    expect(page.summary({ capacityAh: 6, cellsSeries: 6, chemistry: 'liion' })).toBe('6000 mAh · 6S liion');
    expect(page.summary({ massKg: 1.9, cruiseAirspeedMps: 10 })).toBe('1.9 kg · cruise 10 m/s');
    expect(page.summary({ kind: 'control' })).toBe('control');
    expect(page.summary({ id: 'just-an-id' })).toBe('just-an-id');
    expect(page.summary({ kind: { nested: true } })).toBe('');
  });

  it('asks for the admin token when saving without one', async () => {
    await MockApiHarness.settle(page.selectKind('batteries'));
    page.newEntry();
    draft({ ...seeded, id: 'lib-no-admin' });
    expect(page.validation().ok).toBe(true);
    await save();
    expect(page.message()).toEqual({ ok: false, text: 'Set the admin token first (top right).' });
  });

  it('adds an entry, saves a new version and reports a conflict on a stale version', async () => {
    creds.setAdminToken('admin-token');
    await MockApiHarness.settle(page.selectKind('batteries'));
    page.newEntry();
    draft({ ...seeded, id: 'lib-pack' });
    await save();
    expect(page.message()).toEqual({ ok: true, text: 'Saved as version 1.' });
    expect(page.selectedId()).toBe('lib-pack');

    draft({ ...seeded, id: 'lib-pack', version: 1, notes: 'checked' });
    await save();
    expect(page.message()).toEqual({ ok: true, text: 'Saved as version 2.' });

    draft({ ...seeded, id: 'lib-pack', version: 1 });
    await save();
    expect(page.conflict()).toBe(true);
    expect(page.message()?.text).toMatch(/^Someone saved this item after version 1 that you edited\. Nothing was saved\. /);
    expect(fixture.nativeElement.textContent).toContain('Someone saved this item');

    await MockApiHarness.settle(page.loadCurrent());
    expect(page.message()).toEqual({ ok: true, text: 'Loaded version 2. Make your change again and save.' });
    expect(page.conflict()).toBe(false);
  });

  it('shows the plain server message for a duplicate new entry or an unknown item', async () => {
    creds.setAdminToken('admin-token');
    await MockApiHarness.settle(page.selectKind('batteries'));
    page.newEntry();
    draft({ ...LIBRARY.batteries[1] });
    await save();
    expect(page.conflict()).toBe(false);
    expect(page.message()?.text).toContain(`An item with id ${LIBRARY.batteries[1].id} exists`);

    page.selectedId.set('lib-missing');
    draft({ ...seeded, id: 'lib-missing', version: 1 });
    await save();
    expect(page.message()?.text).toContain('No such item');
  });

  it('lists the issues of an entry the server rejects', () => {
    // the client validates with the same schema as the server, so a 422 only comes from a server that differs
    const show = (page as unknown as { showSaveError(e: ApiError, v: Entry): void }).showSaveError.bind(page);
    show(new ApiError(422, 'Schema validation failed', { issues: [{ path: '/massKg', message: 'must be number' }] }), {});
    expect(page.message()).toEqual({ ok: false, text: 'The server rejected the entry.' });
    expect(page.serverIssues()).toEqual([{ path: '/massKg', message: 'must be number' }]);
    fixture.detectChanges();
    expect(fixture.nativeElement.textContent).toContain('must be number');
  });
});
