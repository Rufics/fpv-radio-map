import { TestBed } from '@angular/core/testing';
import { provideRouter } from '@angular/router';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import {
  MemoryOfflineStorage,
  OFFLINE_FETCH,
  OFFLINE_STORAGE,
  OfflineMapSources,
  OfflineRegion,
  RfmapApiClient,
  ZipIndex,
} from '@rfmap/ui-kit';
import { MockApiHarness } from '../tests/mock-api-harness';
import { MockAreas, MockRegions } from '../mock/mock-regions';
import { AreaDrawComponent } from './area-draw.component';
import { OfflineLibrary } from './offline-library.service';
import { OfflineRegionFormat } from './offline-region-format';
import { OfflineRegionsPage } from './offline-regions.page';

const region: OfflineRegion = {
  id: 'r',
  name: 'R',
  bbox: [-3.5, -12.25, 4, 10],
  maxzoom: null,
  layers: ['basemap', 'terrain', 'other'],
  size: 186_400_000,
  version: '0123456789abcdef',
  sha256: 'x',
  builtAt: '2026-10-30T04:12:00Z',
  preview: null,
  package: '/api/v1/regions/r/packages/0123456789abcdef.zip',
};

const field = {
  name: 'Launch field',
  geometry: {
    type: 'Polygon' as const,
    coordinates: [[[35.1, 48.6], [35.15, 48.6], [35.15, 48.65], [35.1, 48.65], [35.1, 48.6]]],
  },
};

function setUp(): MemoryOfflineStorage {
  const storage = new MemoryOfflineStorage(true);
  MockApiHarness.setUp();
  TestBed.configureTestingModule({
    providers: [
      provideRouter([]),
      { provide: OFFLINE_STORAGE, useValue: storage },
      { provide: OFFLINE_FETCH, useValue: MockRegions.fetch },
    ],
  });
  try {
    localStorage.clear();
  } catch {
    // no storage in this environment
  }
  return storage;
}

describe('OfflineRegionFormat', () => {
  it('words size, layers, detail, version, box and area', () => {
    expect(OfflineRegionFormat.size(186_400_000)).toBe('186 MB');
    expect(OfflineRegionFormat.size(1_420_000_000)).toBe('1.4 GB');
    expect(OfflineRegionFormat.size(10)).toBe('1 MB');
    expect(OfflineRegionFormat.layers(region)).toBe('Base map · 3D terrain · other');
    expect(OfflineRegionFormat.zoom(region)).toBe('Full detail');
    expect(OfflineRegionFormat.zoom({ ...region, maxzoom: 14 })).toBe('Up to zoom 14');
    expect(OfflineRegionFormat.version(region)).toBe('01234567');
    expect(OfflineRegionFormat.builtOn(region)).toBe('30 Oct 2026');
    expect(OfflineRegionFormat.builtOn({ ...region, builtAt: 'soon' })).toBe('soon');
    expect(OfflineRegionFormat.bbox(region)).toBe('12.25° S – 10.00° N, 3.50° W – 4.00° E');
    expect(OfflineRegionFormat.km2(0.4219)).toBe('0.42 km²');
    expect(OfflineRegionFormat.km2(2500)).toBe('2,500 km²');
    expect(OfflineRegionFormat.savedOn('later')).toBe('later');
  });
});

describe('OfflineLibrary', () => {
  beforeEach(() => setUp());
  afterEach(() => MockApiHarness.tearDown());

  it('saves a package, says when the catalog has a newer one, and gives the map its tiles', async () => {
    const library = TestBed.inject(OfflineLibrary);
    const ridge = MockRegions.list[0];
    await MockApiHarness.settle(library.save('regions', ridge));
    expect(library.failures()).toEqual({});
    expect(library.saved().map((s) => s.key)).toEqual(['regions/dnipro-ridge']);
    expect(library.active()).toBe('regions/dnipro-ridge');
    expect(library.updateAvailable('regions', ridge)).toBe(false);
    expect(library.updateAvailable('regions', { ...ridge, version: 'f'.repeat(64) })).toBe(true);

    const config = await MockApiHarness.settle(library.mapConfig({ weather: { tiles: ['w'] } }));
    expect(config?.glyphs).toBe('rfoffline://regions/dnipro-ridge/glyphs/{fontstack}/{range}.pbf');
    expect(config?.weather?.tiles).toEqual(['w']);

    await MockApiHarness.settle(library.remove('regions/dnipro-ridge'));
    expect(library.saved()).toEqual([]);
    expect(library.active()).toBeNull();
    expect(await library.mapConfig(null)).toBeNull();
  });

  it('keeps nothing of a damaged download and says why', async () => {
    const library = TestBed.inject(OfflineLibrary);
    const damaged = { ...MockRegions.list[1], sha256: '0'.repeat(64) };
    await MockApiHarness.settle(library.save('regions', damaged));
    expect(library.saved()).toEqual([]);
    expect(library.failures()['regions/dnipro-oblast']).toContain('SHA-256');
    expect(library.saving()).toEqual({});
  });
});

describe('OfflineRegionsPage', () => {
  let storage: MemoryOfflineStorage;
  beforeEach(() => {
    storage = setUp();
    TestBed.overrideComponent(AreaDrawComponent, { set: { template: '', imports: [] } });
  });
  afterEach(() => MockApiHarness.tearDown());

  it('lists what there is to download, saves a region on the device and offers it to the map', async () => {
    const fixture = TestBed.createComponent(OfflineRegionsPage);
    fixture.detectChanges();
    await MockApiHarness.wait(1000);
    fixture.detectChanges();
    const el = fixture.nativeElement as HTMLElement;
    const cards = el.querySelectorAll('ul[aria-label="Offline regions"] li.card');
    expect(cards.length).toBe(3);
    expect(cards[0].querySelector('h2')?.textContent).toContain('Dnipro ridge');
    const link = cards[1].querySelector('a[download]') as HTMLAnchorElement;
    expect(link.getAttribute('href')).toMatch(/^\/api\/v1\/regions\/dnipro-oblast\/packages\/[0-9a-f]{64}\.zip$/);
    expect(link.getAttribute('aria-label')).toMatch(/^Download Dnipro oblast, \d+ MB$/);
    expect(el.querySelector('[role="alert"]')).toBeNull();
    expect(el.textContent).toContain('Nothing saved yet');
    expect(el.querySelector('#draw-h')?.textContent).toContain('Draw an area');

    (cards[0].querySelector('button.rf-btn--primary') as HTMLButtonElement).click();
    await MockApiHarness.wait(500);
    fixture.detectChanges();
    expect(cards[0].textContent).toContain('On this device, saved');
    const saved = el.querySelectorAll('ul[aria-label="Saved packages"] li');
    expect(saved.length).toBe(2); // the server's map, and the region
    expect((saved[1].querySelector('input') as HTMLInputElement).checked).toBe(true);
    const zip = await storage.zip('regions/dnipro-ridge');
    expect((await ZipIndex.read(zip!)).has('manifest.json')).toBe(true);
    OfflineMapSources.close('regions/dnipro-ridge');
  });

  it('follows a drawn area until it is built, then offers its package', async () => {
    const library = TestBed.inject(OfflineLibrary);
    const api = TestBed.inject(RfmapApiClient);
    const asked = await MockApiHarness.settle(api.requestOfflineArea(field));
    library.remember({ id: asked.id, name: field.name, request: field, askedAt: '' });
    const fixture = TestBed.createComponent(OfflineRegionsPage);
    fixture.detectChanges();
    await MockApiHarness.wait(500);
    fixture.detectChanges();
    const el = fixture.nativeElement as HTMLElement;
    const drawn = () => el.querySelector('ul[aria-label="Drawn areas"] li') as HTMLElement;
    expect(drawn().textContent).toMatch(/Waiting|Building/);
    await MockApiHarness.wait(MockAreas.queuedMs + MockAreas.buildMs + 2 * OfflineRegionsPage.pollMs);
    fixture.detectChanges();
    expect(drawn().querySelector('h2')?.textContent).toContain('Launch field');
    expect(drawn().querySelector('a[download]')?.getAttribute('href')).toMatch(/^\/api\/v1\/areas\/area-[0-9a-f]{24}\/packages\//);

    (drawn().querySelector('button[aria-label^="Forget"]') as HTMLButtonElement).click();
    fixture.detectChanges();
    expect(el.querySelector('ul[aria-label="Drawn areas"]')).toBeNull();
  });
});

describe('AreaDrawComponent', () => {
  beforeEach(() => {
    setUp();
    TestBed.overrideComponent(AreaDrawComponent, { set: { template: '', imports: [] } });
  });
  afterEach(() => MockApiHarness.tearDown());

  it('checks the drawn polygon against the limits and asks the server for its package', async () => {
    const fixture = TestBed.createComponent(AreaDrawComponent);
    fixture.componentRef.setInput('limits', MockAreas.limits);
    const draw = fixture.componentInstance;
    const asked: string[] = [];
    draw.asked.subscribe((s) => asked.push(s.id));
    draw.addCorner({ lat: 48.6, lon: 35.1 });
    draw.addCorner({ lat: 48.6, lon: 35.15 });
    expect(draw.ready()).toBe(false);
    draw.addCorner({ lat: 48.65, lon: 35.15 });
    expect(draw.ready()).toBe(true);
    draw.addCorner({ lat: 48.65, lon: 35.1 });
    draw.name.set('Launch field');

    // about 20 km²: over a 10 km² limit
    fixture.componentRef.setInput('limits', { ...MockAreas.limits, maxAreaKm2: 10 });
    expect(draw.ready()).toBe(false);
    fixture.componentRef.setInput('limits', MockAreas.limits);
    expect(draw.ready()).toBe(true);
    // a bow tie has no clear inside
    draw.addCorner({ lat: 48.58, lon: 35.13 });
    expect(draw.ready()).toBe(false);
    draw.shape.set(draw.shape().withoutLast());

    await MockApiHarness.settle(draw.submit());
    expect(asked.length).toBe(1);
    expect(asked[0]).toMatch(/^area-[0-9a-f]{24}$/);
    expect(TestBed.inject(OfflineLibrary).drawn()[0]).toEqual(expect.objectContaining({ id: asked[0], name: 'Launch field' }));
    expect(draw.shape().points.length).toBe(0);
  });
});
