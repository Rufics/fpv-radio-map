import { provideHttpClient } from '@angular/common/http';
import { provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { ModuleInfo } from '../models/api.models';
import { SlotRegistry } from '../slots/slot-registry';
import { MODULE_BUNDLE_ORIGINS, MODULE_IMPORTER, ModuleImporter, ModuleUiLoader } from './module-loader';
import { RfModuleUiApi, RfModuleUiBundle } from './module-api';

function info(over: Partial<ModuleInfo> = {}): ModuleInfo {
  return {
    id: 'fpv.example',
    version: '0.1.0',
    lifecycle: 'beta',
    state: 'active',
    ui: { bundle: '/modules/fpv-example/ui.js', slots: ['side.panels'] },
    ...over,
  };
}

describe('ModuleUiLoader', () => {
  let bundles: Record<string, RfModuleUiBundle | Error>;
  let imported: string[];

  beforeEach(() => {
    bundles = {};
    imported = [];
    const importer: ModuleImporter = async (url) => {
      imported.push(url);
      const b = bundles[new URL(url).pathname];
      if (!b) throw new Error(`404 ${url}`);
      if (b instanceof Error) throw b;
      return b;
    };
    TestBed.configureTestingModule({
      providers: [
        provideHttpClient(),
        provideHttpClientTesting(),
        { provide: MODULE_IMPORTER, useValue: importer },
        { provide: MODULE_BUNDLE_ORIGINS, useValue: ['https://modules.example'] },
      ],
    });
  });

  it('imports the bundle from ModuleInfo.ui.bundle and registers its panel into the slot', async () => {
    bundles['/modules/fpv-example/ui.js'] = {
      kit: '1',
      register(api: RfModuleUiApi) {
        api.slots.register({ id: 'panel', slot: 'side.panels', title: 'Example', mount: (el) => void (el.textContent = 'hi') });
      },
    };
    const loader = TestBed.inject(ModuleUiLoader);
    const status = await loader.load(info());
    expect(status).toEqual({ moduleId: 'fpv.example', state: 'loaded', contributions: 1 });
    expect(imported[0]).toMatch(/\/modules\/fpv-example\/ui\.js$/);
    const slot = TestBed.inject(SlotRegistry).slot('side.panels')();
    expect(slot.map((c) => [c.id, c.source])).toEqual([['fpv.example/panel', 'fpv.example']]);
  });

  it('supports a default export', async () => {
    bundles['/modules/fpv-example/ui.js'] = {
      default: { register: (api) => void api.slots.register({ id: 'p', slot: 'side.panels', mount: () => undefined }) },
    };
    const status = await TestBed.inject(ModuleUiLoader).load(info());
    expect(status.state).toBe('loaded');
  });

  it('refuses slots the module did not declare and removes what it registered', async () => {
    bundles['/modules/fpv-example/ui.js'] = {
      register(api) {
        api.slots.register({ id: 'ok', slot: 'side.panels', mount: () => undefined });
        api.slots.register({ id: 'bad', slot: 'header.actions', mount: () => undefined });
      },
    };
    const status = await TestBed.inject(ModuleUiLoader).load(info());
    expect(status.state).toBe('failed');
    expect(status.state === 'failed' && status.error).toMatch(/header\.actions/);
    expect(TestBed.inject(SlotRegistry).all().length).toBe(0);
  });

  it('isolates import errors and wrong kit majors', async () => {
    bundles['/modules/fpv-example/ui.js'] = new Error('network down');
    const loader = TestBed.inject(ModuleUiLoader);
    expect(await loader.load(info())).toMatchObject({ state: 'failed', error: 'network down' });
    bundles['/modules/fpv-example/ui.js'] = { kit: '2', register: () => undefined };
    expect(await loader.load(info())).toMatchObject({ state: 'failed' });
    bundles['/modules/fpv-example/ui.js'] = { kit: '1' };
    expect(await loader.load(info())).toMatchObject({ state: 'failed', error: 'bundle does not export register()' });
  });

  it('skips inactive and alpha modules, and foreign origins', async () => {
    const loader = TestBed.inject(ModuleUiLoader);
    expect((await loader.load(info({ state: 'failed' }))).state).toBe('skipped');
    expect((await loader.load(info({ lifecycle: 'alpha' }))).state).toBe('skipped');
    const foreign = await loader.load(info({ ui: { bundle: 'https://evil.example/x.js', slots: ['side.panels'] } }));
    expect(foreign).toMatchObject({ state: 'failed' });
    expect(imported.length).toBe(0);
    bundles['/x.js'] = { register: () => undefined };
    const allowed = await loader.load(info({ ui: { bundle: 'https://modules.example/x.js', slots: [] } }));
    expect(allowed.state).toBe('loaded');
  });

  it('unload removes the contributions', async () => {
    bundles['/modules/fpv-example/ui.js'] = {
      register: (api) => void api.slots.register({ id: 'p', slot: 'side.panels', mount: () => undefined }),
    };
    const loader = TestBed.inject(ModuleUiLoader);
    await loader.load(info());
    loader.unload('fpv.example');
    expect(TestBed.inject(SlotRegistry).all().length).toBe(0);
    expect(loader.status().length).toBe(0);
  });

  it('gives the bundle a read-only state view', async () => {
    let seen: unknown = 'unset';
    bundles['/modules/fpv-example/ui.js'] = {
      register: (api) => {
        seen = api.state.mission();
      },
    };
    await TestBed.inject(ModuleUiLoader).load(info());
    expect(seen).toBeNull();
  });
});
