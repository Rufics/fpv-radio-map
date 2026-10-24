import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Marker, type Map as MlMap } from 'maplibre-gl';
import {
  CoverageGrid,
  CoverageProbe,
  LineOfSightLayer,
  MapContext,
  MissionStore,
  provideMapView,
} from '@rfmap/ui-kit';
import { coverageGrid, exampleMission } from '../mock/reference-case';
import { MockApiHarness } from '../tests/mock-api-harness';
import { LineOfSightComponent, SightReadout } from './line-of-sight.component';
import { MiniMapPainter } from './coverage-mini-map.component';
import { MapPointer } from './map-pointer';

describe('SightReadout', () => {
  it('reads out the coverage margin at once and says the exact check is running', () => {
    const r = { marginDb: 12.3, distanceM: 3210 };
    expect(SightReadout.tone(r)).toBe('ok');
    expect(SightReadout.text(r)).toBe('+12.30 dB · 3.2 km · checking…');
  });

  it('says what blocks the line once the path is calculated', () => {
    const exact = SightReadout.fromResult({
      marginDb: -6.5,
      diffraction: {
        lineOfSight: false,
        edgeHeightM: 622,
        dominant: { lat: 48.5, lon: 35.2, distanceM: 2100, groundMslM: 600 },
      },
    });
    expect(exact.exact?.obstruction).toEqual({ lat: 48.5, lon: 35.2 });
    const r = { distanceM: 4000, ...exact };
    expect(SightReadout.tone(r)).toBe('blocked');
    expect(SightReadout.text(r)).toBe('−6.50 dB · 4.0 km · blocked by terrain 622 m');
  });

  it('says the line is clear, and when the check failed', () => {
    const clear = SightReadout.fromResult({ marginDb: 4, diffraction: { lineOfSight: true } });
    expect(clear.exact).toEqual({ lineOfSight: true, edgeHeightM: undefined, obstruction: null });
    expect(SightReadout.text({ distanceM: 1000, ...clear })).toBe(
      '+4.00 dB · 1.0 km · clear line of sight',
    );
    expect(SightReadout.text({ marginDb: null, distanceM: 1000, failed: true })).toBe(
      'no link · 1.0 km · no terrain data',
    );
  });
});

describe('MiniMapPainter.size', () => {
  it('keeps the grid aspect in Mercator inside the box', () => {
    const square = CoverageProbe.of({
      cols: 1,
      rows: 1,
      bbox: [35, 48, 35.2, 48.2],
      marginDb: [1],
    })!;
    const s = MiniMapPainter.size(square, 180);
    // a square in degrees is taller than wide in Mercator at 48° N
    expect(s.height).toBe(180);
    expect(s.width).toBeLessThan(180);
    const wide = CoverageProbe.of({ cols: 1, rows: 1, bbox: [35, 48, 36, 48.2], marginDb: [1] })!;
    expect(MiniMapPainter.size(wide, 180).width).toBe(180);
  });
});

/** A 2D context that records the calls the painter makes. */
function recordingContext(): { ctx: CanvasRenderingContext2D; calls: string[] } {
  const calls: string[] = [];
  const ctx = new Proxy({} as Record<string | symbol, unknown>, {
    get: (target, key) =>
      key in target
        ? target[key]
        : (...args: unknown[]) => {
            calls.push(`${String(key)}(${args.length})`);
          },
    set: (target, key, value) => {
      target[key] = value;
      return true;
    },
  });
  return { ctx: ctx as unknown as CanvasRenderingContext2D, calls };
}

describe('MiniMapPainter.paint', () => {
  it('draws the background, the route, the view and the cursor', () => {
    const { ctx, calls } = recordingContext();
    const canvas = {
      width: 180,
      height: 200,
      getContext: () => ctx,
    } as unknown as HTMLCanvasElement;
    const grid: CoverageGrid = {
      cols: 2,
      rows: 2,
      bbox: [35, 48, 35.2, 48.2],
      marginDb: [1, 2, 3, 4],
    };
    new MiniMapPainter().paint(canvas, grid, CoverageProbe.of(grid)!, {
      route: [
        { lat: 48.05, lon: 35.05 },
        { lat: 48.15, lon: 35.15 },
      ],
      view: [35.02, 48.02, 35.18, 48.18],
      cursor: { lat: 48.1, lon: 35.1 },
    });
    expect(calls[0]).toBe('fillRect(4)');
    // the route is stroked twice (casing, then line), the cursor ring twice, the view once
    expect(calls.filter((c) => c === 'stroke(0)')).toHaveLength(4);
    expect(calls).toContain('strokeRect(4)');
    expect(calls).toContain('arc(5)');
  });

  it('skips the route, view and cursor when there are none', () => {
    const { ctx, calls } = recordingContext();
    const canvas = { width: 90, height: 90, getContext: () => ctx } as unknown as HTMLCanvasElement;
    const g: CoverageGrid = { cols: 1, rows: 1, bbox: [35, 48, 35.2, 48.2], marginDb: [1] };
    new MiniMapPainter().paint(canvas, g, CoverageProbe.of(g)!, {
      route: [],
      view: null,
      cursor: null,
    });
    expect(calls).toEqual(['fillRect(4)']);
  });
});

describe('MapPointer', () => {
  it('follows the mouse and the view, and stops when disconnected', () => {
    const handlers = new Map<string, (e?: unknown) => void>();
    const map = {
      on: (type: string, fn: (e?: unknown) => void) => handlers.set(type, fn),
      off: (type: string) => handlers.delete(type),
      getBounds: () => ({
        getWest: () => 1,
        getSouth: () => 2,
        getEast: () => 3,
        getNorth: () => 4,
      }),
    } as unknown as MlMap;
    const pointer = new MapPointer();
    pointer.connect(map);
    expect(pointer.view()).toEqual([1, 2, 3, 4]);
    handlers.get('mousemove')!({ lngLat: { lat: 48, lng: 35 } });
    expect(pointer.cursor()).toEqual({ lat: 48, lon: 35 });
    handlers.get('mouseout')!();
    expect(pointer.cursor()).toBeNull();
    pointer.disconnect();
    expect(handlers.size).toBe(0);
  });
});

describe('LineOfSightComponent', () => {
  beforeEach(() => {
    MockApiHarness.setUp();
    TestBed.configureTestingModule({ providers: [provideMapView(), MapPointer] });
  });

  afterEach(() => {
    vi.restoreAllMocks();
    MockApiHarness.tearDown();
  });

  it('draws the line at once and says what blocks it after the mouse rests', async () => {
    const sights: unknown[] = [];
    vi.spyOn(LineOfSightLayer.prototype, 'setSight').mockImplementation((s) => void sights.push(s));
    vi.spyOn(Marker.prototype, 'addTo').mockReturnThis();
    const mission = exampleMission();
    TestBed.inject(MissionStore).newMission(mission);
    TestBed.inject(MapContext).map.set({
      project: () => ({ x: 900, y: 100 }),
      getContainer: () => ({ clientWidth: 1000 }),
    } as unknown as MlMap);
    const fixture = TestBed.createComponent(LineOfSightComponent);
    fixture.componentRef.setInput('active', true);
    fixture.componentRef.setInput('grid', coverageGrid(mission, 'control'));
    const pointer = TestBed.inject(MapPointer);
    const target = { lat: 48.5, lon: 35.4 };
    pointer.cursor.set(target);
    TestBed.tick();
    expect(sights.at(-1)).toMatchObject({ to: target, obstruction: null });
    const tag = () => findTag(fixture);
    expect(tag()?.textContent).toContain('checking…');
    // a provisional reading is not announced
    const live = () => fixture.nativeElement.querySelector('[role="status"]') as HTMLElement;
    fixture.detectChanges();
    expect(live().textContent).toBe('');

    await MockApiHarness.wait(3000);
    expect(tag()?.textContent).not.toContain('checking…');
    fixture.detectChanges();
    expect(live().textContent).toBe(tag()?.textContent);

    pointer.cursor.set(null);
    TestBed.tick();
    expect(sights.at(-1)).toBeNull();
  });
});

/** The tag lives in a detached marker element (the fake map adds nothing to the page). */
function findTag(fixture: { componentInstance: unknown }): Element | null {
  const marker = (fixture.componentInstance as { tag?: Marker }).tag;
  return marker?.getElement().firstElementChild ?? null;
}
