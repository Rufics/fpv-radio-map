import { TestBed } from '@angular/core/testing';
import { provideRouter } from '@angular/router';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { OfflineRegion } from '@rfmap/ui-kit';
import { MockApiHarness } from '../tests/mock-api-harness';
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

describe('OfflineRegionFormat', () => {
  it('words size, layers, detail, version and box', () => {
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
  });
});

describe('OfflineRegionsPage', () => {
  beforeEach(() => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideRouter([])] });
  });
  afterEach(() => MockApiHarness.tearDown());

  it('lists what there is to download, each with its size and versioned package link', async () => {
    const fixture = TestBed.createComponent(OfflineRegionsPage);
    fixture.detectChanges();
    await MockApiHarness.wait(1000);
    fixture.detectChanges();
    const el = fixture.nativeElement as HTMLElement;
    const cards = el.querySelectorAll('li.card');
    expect(cards.length).toBe(3);
    expect(cards[0].querySelector('h2')?.textContent).toContain('Dnipro ridge');
    const link = cards[1].querySelector('a[download]') as HTMLAnchorElement;
    expect(link.getAttribute('href')).toMatch(
      /^\/api\/v1\/regions\/dnipro-oblast\/packages\/[0-9a-f]{64}\.zip$/,
    );
    expect(link.getAttribute('aria-label')).toBe('Download Dnipro oblast, 1.4 GB');
    expect(el.querySelector('[role="alert"]')).toBeNull();
  });
});
