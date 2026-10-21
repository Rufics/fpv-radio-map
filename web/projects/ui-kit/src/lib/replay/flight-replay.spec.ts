import { FlightTimeline, FlightView } from '../models/flightlog.models';
import { ReplayFormat, ReplayTimeline, TrackScale } from './flight-replay';

/** Five samples at 1 Hz with a GPS dropout at t=2 and the cell voltage falling through 3.5 V. */
function view(channels: FlightTimeline['channels'] = {}): FlightView {
  return {
    schema: 'flightview/v1',
    index: 1,
    status: 'ok',
    durationS: 4,
    firmware: { name: 'Betaflight', version: '4.5.1', supported: true },
    summary: { durationS: 4, armedS: 4 },
    bands: [
      { from: 0, to: 2, band: 'ANGLE' },
      { from: 2, to: 4, band: 'FAILSAFE' },
    ],
    warnings: [],
    channels: {},
    notes: [],
    homes: [],
    story: [],
    timeline: {
      hz: 1,
      t: [0, 1, 2, 3, 4],
      channels: {
        lat: [50, 50.001, null, 50.003, 50.004],
        lon: [16, 16.001, null, 16.003, 16.004],
        cellV: [3.9, 3.85, 3.6, 3.4, 3.2],
        sats: [12, 12, 12, 12, 12],
        ...channels,
      },
    },
  };
}

describe('TrackScale', () => {
  it('puts values in classes by the upper bounds', () => {
    expect(TrackScale.classOf('battery', 3.2)).toBe(0);
    expect(TrackScale.classOf('battery', 3.3)).toBe(1);
    expect(TrackScale.classOf('battery', 3.9)).toBe(3);
    expect(TrackScale.classOf('battery', null)).toBe(-1);
    expect(TrackScale.classOf('battery', Number.NaN)).toBe(-1);
    expect(TrackScale.color('link', undefined)).toBe('#5B6878');
  });

  it('lists the legend from the best class down', () => {
    const legend = TrackScale.legend('link');
    expect(legend.map((l) => l.label)).toEqual(['≥ 60 %', '30–60 %', '< 30 %']);
  });

  it('colours the link by link quality when the log has it, else by RSSI', () => {
    expect(TrackScale.channel('link', view().timeline)).toBe('rssiPct');
    expect(TrackScale.channel('link', view({ linkQualityPct: [100, 90, 80, 70, 60] }).timeline)).toBe('linkQualityPct');
  });

  it('offers only the metrics the log has data for', () => {
    const tl = view({ altM: [null, null, null, null, null] }).timeline;
    expect(TrackScale.METRICS.filter((m) => TrackScale.available(m, tl))).toEqual(['battery', 'gps']);
  });
});

describe('ReplayTimeline', () => {
  it('interpolates between samples and keeps a gap a gap', () => {
    const tl = new ReplayTimeline(view());
    expect(tl.start).toBe(0);
    expect(tl.end).toBe(4);
    expect(tl.duration).toBe(4);
    expect(tl.value('cellV', 0.5)).toBeCloseTo(3.875);
    expect(tl.position(2.5)).toBeNull();
    expect(tl.position(1.5)).toEqual({ lat: 50.001, lon: 16.001 });
    expect(tl.value('altM', 1)).toBeNull();
  });

  it('clamps the index to the samples', () => {
    const tl = new ReplayTimeline(view());
    expect(tl.index(-5)).toBe(0);
    expect(tl.index(99)).toBe(4);
  });

  it('reads the mode band at a time, and the last band past the end', () => {
    const tl = new ReplayTimeline(view());
    expect(tl.band(1)).toBe('ANGLE');
    expect(tl.band(2)).toBe('FAILSAFE');
    expect(tl.band(10)).toBe('FAILSAFE');
    const r = tl.reading(0);
    expect(r.values.cellV).toBe(3.9);
    expect(r.position).toEqual({ lat: 50, lon: 16 });
  });

  it('lists positions and splits the track by colour class and at dropouts', () => {
    const tl = new ReplayTimeline(view());
    expect(tl.hasTrack).toBe(true);
    expect(tl.positions()).toHaveLength(4);
    const segments = tl.segments('battery');
    // 3.9 and 3.85 are one class; after the dropout 3.4 and 3.2 are two more, joined end to start
    expect(segments).toHaveLength(2);
    expect(segments[0].points).toHaveLength(2);
    expect(segments[1].points[0]).toEqual({ lat: 50.003, lon: 16.003 });
    expect(segments[1].color).toBe(TrackScale.color('battery', 3.2));
  });

  it('copes with a log that has no samples', () => {
    const empty = view();
    empty.timeline = { hz: 1, t: [], channels: {} };
    const tl = new ReplayTimeline(empty);
    expect(tl.duration).toBe(0);
    expect(tl.hasTrack).toBe(false);
    expect(tl.index(3)).toBe(0);
    expect(tl.segments('battery')).toEqual([]);
  });
});

describe('ReplayFormat', () => {
  it('formats the clock past an hour', () => {
    expect(ReplayFormat.clock(62)).toBe('01:02');
    expect(ReplayFormat.clock(3723)).toBe('1:02:03');
    expect(ReplayFormat.clock(-4)).toBe('00:00');
  });

  it('formats distances, speeds and numbers, with a dash for no data', () => {
    expect(ReplayFormat.distance(640.4)).toBe('640 m');
    expect(ReplayFormat.distance(2310)).toBe('2.31 km');
    expect(ReplayFormat.distance(null)).toBe('–');
    expect(ReplayFormat.speed(10)).toBe('36 km/h');
    expect(ReplayFormat.speed(undefined)).toBe('–');
    expect(ReplayFormat.fixed(3.456, 2, 'V')).toBe('3.46 V');
    expect(ReplayFormat.fixed(3, 0, '')).toBe('3');
    expect(ReplayFormat.fixed(Number.POSITIVE_INFINITY, 1, 'V')).toBe('–');
  });

  it('names the mode bands', () => {
    expect(ReplayFormat.band('GPS_RESCUE')).toBe('GPS Rescue');
    expect(ReplayFormat.bandColor('FAILSAFE')).toBe('#FF6B5B');
  });

  it('formats the start time in UTC and says when the log has no clock', () => {
    expect(ReplayFormat.started('2025-02-02T15:15:20.000+00:00')).toBe('Sun 2 Feb 2025, 15:15 UTC');
    expect(ReplayFormat.started(undefined)).toBe('start time not logged');
    expect(ReplayFormat.started('garbage')).toBe('start time not logged');
  });
});
