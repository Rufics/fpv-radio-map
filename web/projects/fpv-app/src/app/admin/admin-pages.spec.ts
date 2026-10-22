import { Type } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { CredentialStore, ModuleInfo, provideMapView } from '@rfmap/ui-kit';
import { MockApiHarness } from '../tests/mock-api-harness';
import { ModulesPage } from './modules.page';
import { SourcesPage } from './sources.page';

interface ModulesVm {
  modules(): ModuleInfo[];
  keys(): { id: string }[];
  notices(): Record<string, unknown>[];
  error(): string | null;
  keyError(): string | null;
  reloadedAt(): number;
  reload(): Promise<void>;
  toggle(m: ModuleInfo): Promise<void>;
  notes(m: ModuleInfo): string;
  noticeText(n: Record<string, unknown>): string;
  reloadedAgo(): string;
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

  it('modules: loads the trusted keys once the admin token is set', async () => {
    const p = page<ModulesVm>(ModulesPage);
    await MockApiHarness.wait(500);
    expect(p.keys()).toEqual([]);
    creds.setAdminToken('admin-token');
    TestBed.tick();
    await MockApiHarness.wait(500);
    expect(p.keys().length).toBeGreaterThan(0);
    expect(p.keyError()).toBeNull();
  });

  it('modules: toggling needs the admin token and then changes the state', async () => {
    const p = page<ModulesVm>(ModulesPage);
    await MockApiHarness.wait(500);
    const target = p.modules().find((m) => m.id === 'fpv.core')!;
    await MockApiHarness.settle(p.toggle(target));
    expect(p.error()).toContain('admin token');
    creds.setAdminToken('admin-token');
    await MockApiHarness.settle(p.toggle(target));
    expect(p.error()).toBeNull();
    expect(p.modules().find((m) => m.id === 'fpv.core')?.state).toBe('disabled');
    await MockApiHarness.settle(p.toggle(p.modules().find((m) => m.id === 'fpv.core')!));
    expect(p.modules().find((m) => m.id === 'fpv.core')?.state).toBe('active');
  });

  it('modules: words the notes, the notices and the reload time', async () => {
    const p = page<ModulesVm>(ModulesPage);
    expect(p.notes({ id: 'a', deprecation: { since: '1.2.0', removedIn: '2.0.0', replacement: 'b' } })).toBe('Since 1.2.0, removed in 2.0.0. Use b.');
    expect(p.notes({ id: 'a', deprecation: { message: 'Gone soon' } })).toBe('Gone soon');
    expect(p.notes({ id: 'a', stateDetail: 'Disabled by the admin' })).toBe('Disabled by the admin');
    expect(p.notes({ id: 'a', builtIn: true })).toBe('Built in · can be disabled, not removed');
    expect(p.notes({ id: 'a', lifecycle: 'beta' })).toBe('Users see a beta label');
    expect(p.notes({ id: 'a', lifecycle: 'alpha' })).toBe('Hidden from users');
    expect(p.notes({ id: 'a', ui: { bundle: '/m/ui.js', slots: ['side.panels'] } })).toBe('UI bundle · slots side.panels');
    expect(p.notes({ id: 'a' })).toBe('');
    expect(p.noticeText({ module: 'acme.rain', message: 'failed' })).toBe('acme.rain: failed');
    expect(p.noticeText({ code: 7 })).toBe('{"code":7}');
    expect(p.reloadedAgo()).toMatch(/^\d+ s ago$/);
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
