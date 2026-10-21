import { describe, expect, it } from 'vitest';
import { FlightLogRow, PlanFactRow, ReplayTimeline } from '@rfmap/ui-kit';
import { MockFlights } from '../mock/mock-flights';
import { CHART_W, ReplayChart } from './replay-chart';
import { ReplayVm } from './replay-vm';

function row(p: Partial<FlightLogRow> = {}, summary: Partial<FlightLogRow['summary']> = {}): FlightLogRow {
  return {
    index: 0,
    status: 'ok',
    durationS: 462,
    firmware: { name: 'Betaflight', version: '4.5.1', supported: true },
    summary: { durationS: 462, armedS: 450, ...summary },
    bands: [],
    warnings: [],
    ...p,
  };
}

describe('ReplayVm', () => {
  it('sums a flight up in one line with only what the log has', () => {
    expect(ReplayVm.headline(row({}, { distanceM: 4860, usedMah: 1840.4 }))).toBe('07:42 · 4.86 km · 1,840 mAh');
    expect(ReplayVm.headline(row())).toBe('07:42');
    expect(ReplayVm.title(row({ fileName: 'a.bbl' }))).toBe('a.bbl');
    expect(ReplayVm.title(row({ craft: 'LR4', fileName: 'a.bbl' }))).toBe('LR4');
    expect(ReplayVm.firmware(row())).toBe('Betaflight 4.5.1');
    expect(ReplayVm.firmware(row({ firmware: { name: '', supported: false } }))).toBe('unknown firmware');
  });

  it('flags a low cell and a weak link in the summary', () => {
    const items = ReplayVm.summary(row({}, { minCellV: 3.41, minLinkPct: 22, linkChannel: 'linkQualityPct', usedMah: 900, usedWh: 13.27 }));
    expect(items.find((i) => i.label === 'Lowest cell')).toEqual({ label: 'Lowest cell', value: '3.41 V', tone: 'warn' });
    expect(items.find((i) => i.label === 'Lowest link quality')?.tone).toBe('warn');
    expect(items.find((i) => i.label === 'Used')?.value).toBe('900 mAh · 13.3 Wh');
    expect(ReplayVm.summary(row({}, { minLinkPct: 80 })).find((i) => i.label === 'Lowest RSSI')?.tone).toBeUndefined();
  });

  it('counts the logbook without header-only files', () => {
    const totals = ReplayVm.totals([
      row({ warnings: ['Failsafe'] }, { armedS: 3700, distanceM: 1000 }),
      row({}, { armedS: 120, distanceM: 500 }),
      row({ status: 'header-only' }, { armedS: 0 }),
    ]);
    expect(totals.map((t) => t.value)).toEqual(['2', '1 h 4 m', '1.50 km', '1']);
    expect(totals[3]).toMatchObject({ label: 'flight with a failsafe', tone: 'warn' });
    expect(ReplayVm.emptyReason(row({ status: 'header-only' }))).toBe('No flight data, header only');
    expect(ReplayVm.emptyReason(row({ status: 'unsupported' }))).toBe('Format not supported');
    expect(ReplayVm.emptyReason(row())).toBeNull();
  });

  it('reads out the flight at the replay time', () => {
    const view = MockFlights.flight('a.bbl', null);
    const tl = new ReplayTimeline(view);
    const tiles = ReplayVm.readout(view, tl.reading(tl.end / 2));
    expect(tiles.map((t) => t.label)).toEqual(['From home', 'Ground speed', 'Altitude', 'Battery', 'Link quality', 'GPS', 'Mode']);
    expect(tiles.find((t) => t.label === 'GPS')?.value).toBe('15 sats');
  });

  it('writes plan against fact in each unit', () => {
    const r = (p: Partial<PlanFactRow>): PlanFactRow => ({ metric: 'distance', label: '', unit: '', plan: 1200, fact: 1500, ...p });
    expect(ReplayVm.planValue(r({}), 'plan')).toBe('1.20 km');
    expect(ReplayVm.planValue(r({ metric: 'energy', fact: 5.04 }), 'fact')).toBe('5.0 Wh');
    expect(ReplayVm.planValue(r({ metric: 'whPerKm', plan: 3.33 }), 'plan')).toBe('3.3 Wh/km');
    expect(ReplayVm.planValue(r({ metric: 'weakestLink', plan: -2.04 }), 'plan')).toBe('−2.0 dB');
    expect(ReplayVm.planValue(r({ metric: 'weakestLink', fact: 31.6 }), 'fact')).toBe('32 %');
    expect(ReplayVm.planValue(r({ metric: 'maxOffRoute', plan: 0 }), 'plan')).toBe('–');
    expect(ReplayVm.planValue(r({ fact: null }), 'fact')).toBe('–');
    expect(ReplayVm.planDelta(r({ deltaPct: 25.2 }))).toBe('+25 %');
    expect(ReplayVm.planDelta(r({ deltaPct: -7.6 }))).toBe('−8 %');
    expect(ReplayVm.planDelta(r({}))).toBe('');
  });
});

describe('ReplayChart', () => {
  it('maps time to the chart width and back', () => {
    const c = new ReplayChart(10, 20);
    expect(c.x(15)).toBe(CHART_W / 2);
    expect(c.timeAt(0.25)).toBe(12.5);
    expect(c.timeAt(2)).toBe(20);
    expect(new ReplayChart(5, 5).x(5)).toBe(0);
  });

  it('draws bands and story ticks', () => {
    const c = new ReplayChart(0, 100);
    expect(c.bands([{ from: 0, to: 50, band: 'ANGLE' }])).toEqual([{ x: 0, w: 500, color: '#35D0BA', label: 'Angle' }]);
    expect(c.ticks([{ t: 62, type: 'failsafe', severity: 'blocked', title: 'Failsafe' }])).toEqual([{ x: 620, severity: 'blocked', title: '01:02 Failsafe' }]);
  });

  it('breaks the line at missing values and fits or fixes the scale', () => {
    const c = new ReplayChart(0, 4);
    const fit = c.path([0, 1, 2, 3, 4], [0, 10, null, 5, 10], 30);
    expect(fit).toEqual({ d: 'M0.0 30.0L250.0 0.0M750.0 15.0L1000.0 0.0', min: 0, max: 10 });
    const fixed = c.path([0, 1], [50, 100], 30, [0, 100]);
    expect(fixed.d).toBe('M0.0 15.0L250.0 0.0');
    expect(c.path([0], [null], 30)).toEqual({ d: '', min: 0, max: 0 });
  });

  it('thins long series to at most the given points', () => {
    const t = Array.from({ length: 1000 }, (_, i) => i / 250);
    const d = new ReplayChart(0, 4).path(t, t, 30, undefined, 100).d;
    expect(d.match(/[ML]/g)).toHaveLength(100);
  });
});
