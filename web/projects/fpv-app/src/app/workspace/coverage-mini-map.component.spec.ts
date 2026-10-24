import { TestBed } from '@angular/core/testing';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { Map as MlMap } from 'maplibre-gl';
import { CoverageGrid, MapContext, provideMapView } from '@rfmap/ui-kit';
import { CoverageMiniMapComponent } from './coverage-mini-map.component';
import { MapPointer } from './map-pointer';

describe('CoverageMiniMapComponent', () => {
  const grid: CoverageGrid = { cols: 2, rows: 1, bbox: [35, 48, 35.4, 48.2], marginDb: [8, -3] };
  let easeTo: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    TestBed.configureTestingModule({ providers: [provideMapView(), MapPointer] });
    easeTo = vi.fn();
    TestBed.inject(MapContext).map.set({ easeTo } as unknown as MlMap);
  });

  function render() {
    const fixture = TestBed.createComponent(CoverageMiniMapComponent);
    fixture.componentRef.setInput('grid', grid);
    fixture.componentRef.setInput('route', [
      { lat: 48.1, lon: 35.1 },
      { lat: 48.15, lon: 35.3 },
    ]);
    fixture.detectChanges();
    const el = fixture.nativeElement as HTMLElement;
    return { fixture, el, caption: () => el.querySelector('.cap b')?.textContent?.trim() };
  }

  it('reads out the margin under the mouse', () => {
    const { fixture, el, caption } = render();
    expect(el.querySelector('canvas')?.style.width).toBe('180px');
    expect(caption()).toBe('hover the map');
    const pointer = TestBed.inject(MapPointer);
    pointer.cursor.set({ lat: 48.1, lon: 35.1 });
    fixture.detectChanges();
    expect(caption()).toBe('+8.00 dB');
    pointer.cursor.set({ lat: 49, lon: 36 });
    fixture.detectChanges();
    expect(caption()).toBe('no link');
  });

  it('recentres the main map where it is clicked', () => {
    const { el } = render();
    const canvas = el.querySelector('canvas')!;
    vi.spyOn(canvas, 'getBoundingClientRect').mockReturnValue({
      left: 0,
      top: 0,
      width: 180,
      height: 90,
    } as DOMRect);
    canvas.dispatchEvent(new MouseEvent('click', { clientX: 45, clientY: 45 }));
    const { center } = easeTo.mock.calls[0][0] as { center: [number, number] };
    expect(center[0]).toBeCloseTo(35.1, 5);
    expect(center[1]).toBeGreaterThan(48);
    expect(center[1]).toBeLessThan(48.2);
  });

  it('shows nothing without a grid', () => {
    const fixture = TestBed.createComponent(CoverageMiniMapComponent);
    fixture.detectChanges();
    expect((fixture.nativeElement as HTMLElement).querySelector('canvas')).toBeNull();
  });
});
