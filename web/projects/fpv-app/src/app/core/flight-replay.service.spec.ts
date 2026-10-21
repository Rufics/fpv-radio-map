import { signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { FlightLogRow, FlightView, MissionStore, RfmapApiClient } from '@rfmap/ui-kit';
import { MockFlights } from '../mock/mock-flights';
import { FlightReplay } from './flight-replay.service';

/** The replay state against a stub API: the logbook, the clock and stepping through the story. */
describe('FlightReplay', () => {
  let replay: FlightReplay;
  let flight: FlightView;
  let rows: FlightLogRow[];
  const api = {
    listFlightLogs: vi.fn(async () => rows),
    getFlightLog: vi.fn(async (_m: string, id: string) => ({ ...flight, id })),
    deleteFlightLog: vi.fn(async () => undefined),
  };

  beforeEach(() => {
    flight = MockFlights.flight('a.bbl', null);
    rows = [];
    TestBed.configureTestingModule({
      providers: [
        { provide: RfmapApiClient, useValue: api },
        { provide: MissionStore, useValue: { missionId: signal('m1'), canEdit: signal(true) } },
      ],
    });
    replay = TestBed.inject(FlightReplay);
  });

  afterEach(() => {
    replay.pause();
    vi.clearAllMocks();
  });

  it('opens the import dialog when the mission has no flights yet', async () => {
    await replay.setMode('replay');
    expect(replay.importOpen()).toBe(true);
    expect(replay.view()).toBeNull();
    await replay.setMode('plan');
    expect(replay.mode()).toBe('plan');
  });

  it('opens the newest kept flight when switching to replay', async () => {
    rows = [MockFlights.row({ ...flight, id: 'f1' })];
    await replay.setMode('plan-fact');
    expect(api.getFlightLog).toHaveBeenCalledWith('m1', 'f1');
    expect(replay.stored()).toBe(true);
    expect(replay.mode()).toBe('plan-fact');
  });

  it('shows a flight from the start and keeps the clock inside it', () => {
    replay.show(flight);
    expect(replay.mode()).toBe('replay');
    expect(replay.t()).toBe(0);
    expect(replay.metrics()).toEqual(['battery', 'altitude', 'speed', 'link', 'gps']);
    expect(replay.attention()).toBe(3);
    replay.seek(-10);
    expect(replay.t()).toBe(0);
    replay.seek(1e9);
    expect(replay.t()).toBe(replay.timeline()?.end);
  });

  it('falls back to a track colour the flight has data for', () => {
    replay.colourBy.set('link');
    replay.show({ ...flight, timeline: { ...flight.timeline, channels: { lat: flight.timeline.channels.lat, lon: flight.timeline.channels.lon, sats: flight.timeline.channels.sats } } });
    expect(replay.colourBy()).toBe('gps');
  });

  it('steps through the story events and seeks to each', () => {
    replay.show(flight);
    replay.step(1);
    expect(replay.selected()).toBe(0);
    replay.step(-1);
    expect(replay.selected()).toBe(0);
    replay.selectEvent(3);
    expect(replay.t()).toBeCloseTo(flight.story[3].t);
    expect(replay.selectedEvent()?.type).toBe('failsafe');
    replay.step(-1);
    expect(replay.selected()).toBe(2);
    replay.selectEvent(99);
    expect(replay.selected()).toBe(2);
  });

  it('starts over when played at the end, and pauses', () => {
    replay.show(flight);
    replay.seek(1e9);
    replay.play();
    expect(replay.playing()).toBe(true);
    expect(replay.t()).toBe(0);
    replay.toggle();
    expect(replay.playing()).toBe(false);
  });

  it('clears the flight on screen when it is deleted', async () => {
    replay.show({ ...flight, id: 'f1' });
    await replay.remove('f1');
    expect(api.deleteFlightLog).toHaveBeenCalledWith('m1', 'f1');
    expect(replay.view()).toBeNull();
  });
});
