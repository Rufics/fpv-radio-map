import { signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import type { Map as MlMap } from 'maplibre-gl';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { FlightView, LayerRegistry, MapContext, MissionStore, RfmapApiClient, provideMapView } from '@rfmap/ui-kit';
import { FlightReplay } from '../core/flight-replay.service';
import { MockFlights } from '../mock/mock-flights';
import { FlightReplayMapComponent } from './flight-replay-map.component';

interface MapVm {
  shown: () => boolean;
  title: () => string;
  legend: () => { label: string }[];
}

/** The flown track on the map: shown in replay, story markers, and fitted to the flight. */
describe('FlightReplayMapComponent', () => {
  let replay: FlightReplay;
  let flight: FlightView;
  const placed: HTMLElement[] = [];
  const fakeMap = { fitBounds: vi.fn() } as unknown as MlMap & { fitBounds: ReturnType<typeof vi.fn> };

  beforeEach(() => {
    flight = MockFlights.flight('a.bbl', null);
    TestBed.configureTestingModule({
      providers: [
        provideMapView(),
        { provide: RfmapApiClient, useValue: {} },
        { provide: MissionStore, useValue: { missionId: signal('m1'), canEdit: signal(true) } },
      ],
    });
    TestBed.overrideComponent(FlightReplayMapComponent, { set: { template: '', imports: [] } });
    // markers go into a plain list instead of MapLibre
    vi.spyOn(FlightReplayMapComponent.prototype as unknown as { placeMarker: (m: MlMap, el: HTMLElement) => unknown }, 'placeMarker').mockImplementation(
      (_m, el) => {
        placed.push(el);
        return { remove: () => placed.splice(placed.indexOf(el), 1) };
      },
    );
    replay = TestBed.inject(FlightReplay);
  });

  afterEach(() => {
    placed.length = 0;
    vi.restoreAllMocks();
    fakeMap.fitBounds.mockClear();
  });

  it('draws the track and its story markers only while a flight is replayed', () => {
    const fixture = TestBed.createComponent(FlightReplayMapComponent);
    const vm = fixture.componentInstance as unknown as MapVm;
    const layers = TestBed.inject(LayerRegistry);
    expect(layers.layers().some((l) => l.def.id === 'mission-flight-track')).toBe(true);
    TestBed.inject(MapContext).map.set(fakeMap);
    TestBed.tick();
    expect(vm.shown()).toBe(false);
    expect(placed).toEqual([]);

    replay.show(flight);
    TestBed.tick();
    expect(vm.shown()).toBe(true);
    expect(vm.title()).toBe('Cell voltage');
    expect(vm.legend().length).toBeGreaterThan(1);
    const located = flight.story.filter((e) => e.lat !== undefined && e.lon !== undefined);
    expect(placed).toHaveLength(located.length);
    expect(fakeMap.fitBounds).toHaveBeenCalledTimes(1);

    placed[0].click();
    expect(replay.selected()).toBe(flight.story.indexOf(located[0]));
    TestBed.tick();
    expect(placed.filter((el) => el.className.includes('is-selected'))).toHaveLength(1);

    replay.mode.set('plan');
    TestBed.tick();
    expect(placed).toEqual([]);
    fixture.destroy();
    expect(layers.layers().some((l) => l.def.id === 'mission-flight-track')).toBe(false);
  });

  it('thins a long track to the points it fits the map to', () => {
    const points = Array.from({ length: 4001 }, (_, i) => ({ lat: i, lon: i }));
    const outline = FlightReplayMapComponent.outline(points);
    expect(outline.length).toBeLessThanOrEqual(2001);
    expect(outline.at(-1)).toEqual({ lat: 4000, lon: 4000 });
    expect(FlightReplayMapComponent.outline(points.slice(0, 3))).toHaveLength(3);
  });
});
