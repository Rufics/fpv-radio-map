import { ComponentFixture, TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { CredentialStore, MissionStore, ModuleUiLoader, RfmapApiClient } from '@rfmap/ui-kit';
import { exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { WorkspacePage } from './workspace.page';

/** The workspace page opening missions from the dev mock backend (layouts not rendered). */
describe('WorkspacePage', () => {
  let fixture: ComponentFixture<WorkspacePage>;

  async function open(id: string): Promise<string> {
    fixture = TestBed.createComponent(WorkspacePage);
    fixture.componentRef.setInput('id', id);
    fixture.detectChanges();
    await MockApiHarness.wait(1_000);
    fixture.detectChanges();
    return (fixture.nativeElement as HTMLElement).textContent?.trim() ?? '';
  }

  beforeEach(() => {
    MockApiHarness.setUp();
    vi.stubGlobal('matchMedia', (q: string) => ({ matches: false, media: q, addEventListener: () => undefined, removeEventListener: () => undefined }));
    TestBed.overrideComponent(WorkspacePage, { set: { template: '@if (loadError(); as e) {<p>{{ e }}</p>}', imports: [] } });
    // module UI bundles are not part of this test
    vi.spyOn(TestBed.inject(ModuleUiLoader), 'loadAll').mockResolvedValue(undefined as never);
  });

  afterEach(() => {
    fixture.destroy();
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
    MockApiHarness.tearDown();
  });

  it('opens a mission this browser has a session for', async () => {
    const created = await MockApiHarness.settle(TestBed.inject(RfmapApiClient).createMission(exampleMission()));
    TestBed.inject(CredentialStore).setSession(created.missionId, created.session);
    expect(await open(created.missionId)).toBe('');
    expect(TestBed.inject(MissionStore).missionId()).toBe(created.missionId);
  });

  it('asks for the shared link again without a session', async () => {
    expect(await open('m-north-ridge')).toBe('This browser has no session for the mission. Open the shared link again and enter the PIN.');
  });

  it('shows the server message for an unknown mission', async () => {
    const text = await open('m-does-not-exist');
    expect(text.length).toBeGreaterThan(0);
    expect(text).not.toContain('session');
  });
});
