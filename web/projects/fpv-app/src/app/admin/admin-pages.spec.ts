import { Type } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { CredentialStore, ModuleInfo, provideMapView } from '@rfmap/ui-kit';
import { MockApiHarness } from '../tests/mock-api-harness';
import { ModulesPage } from './modules.page';
import { SourcesPage } from './sources.page';

interface ModulesVm {
  modules(): ModuleInfo[];
  error(): string | null;
  reloadedAt(): number;
  reload(): Promise<void>;
}

interface SourcesVm {
  tiers(): unknown[];
  cacheError(): string | null;
}

/** Admin pages loading from the dev mock backend (templates not rendered: the sources page has a map). */
describe('admin pages', () => {
  let creds: CredentialStore;

  function page<T>(component: Type<unknown>): T {
    const fixture = TestBed.createComponent(component);
    fixture.detectChanges();
    return fixture.componentInstance as T;
  }

  beforeEach(() => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView()] });
    TestBed.overrideComponent(ModulesPage, { set: { template: '' } });
    TestBed.overrideComponent(SourcesPage, { set: { template: '', imports: [] } });
    creds = TestBed.inject(CredentialStore);
  });

  afterEach(() => {
    creds.setAdminToken(null);
    MockApiHarness.tearDown();
  });

  it('modules: lists the installed modules on open', async () => {
    const p = page<ModulesVm>(ModulesPage);
    await MockApiHarness.wait(500);
    expect(p.error()).toBeNull();
    expect(p.modules().some((m) => m.id === 'fpv.core')).toBe(true);
  });

  it('modules: reload needs the admin token and then refreshes the listing', async () => {
    const p = page<ModulesVm>(ModulesPage);
    await MockApiHarness.settle(p.reload());
    expect(p.error()).toContain('admin token');
    expect(p.reloadedAt()).toBe(0);
    creds.setAdminToken('admin-token');
    await MockApiHarness.settle(p.reload());
    expect(p.error()).toBeNull();
    expect(p.reloadedAt()).toBeGreaterThan(0);
    expect(p.modules().some((m) => m.id === 'fpv.core')).toBe(true);
  });

  it('sources: asks for the admin token before it shows cache limits', async () => {
    const p = page<SourcesVm>(SourcesPage);
    await MockApiHarness.wait(1_000);
    expect(p.cacheError()).toBe('Set the admin token (top right) to see and change cache limits.');
    expect(p.tiers()).toEqual([]);
  });

  it('sources: shows the cache tiers to the admin', async () => {
    creds.setAdminToken('admin-token');
    const p = page<SourcesVm>(SourcesPage);
    await MockApiHarness.wait(1_000);
    expect(p.cacheError()).toBeNull();
    expect(p.tiers().length).toBeGreaterThan(0);
  });
});
