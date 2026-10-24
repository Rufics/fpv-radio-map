import { describe, expect, it } from 'vitest';
import { Corridor } from '../core/fpv-altitude';
import { corridorOf } from '../tests/altitude-fixtures';
import { AltitudeChart, PROFILE_H, PROFILE_W } from './altitude-chart';

describe('AltitudeChart', () => {
  const corridor = new Corridor(corridorOf(), { lat: 48.5, lon: 35.2 });
  const chart = new AltitudeChart(corridor, true);

  it('scales distance to the width and heights to the padded range', () => {
    expect(chart.x(0)).toBe(0);
    expect(chart.x(3000)).toBe(PROFILE_W);
    expect(chart.min).toBeLessThan(450);
    expect(chart.max).toBeGreaterThan(710);
    expect(chart.y(chart.min)).toBe(PROFILE_H);
    expect(chart.y(chart.max)).toBe(0);
    expect(chart.sAt(0.5)).toBe(1500);
    expect(chart.sAt(2)).toBe(3000);
    // Without the corridor the ceiling does not stretch the range.
    expect(new AltitudeChart(corridor, false).max).toBeLessThan(chart.max);
  });

  it('cuts the plan where its state changes', () => {
    expect(chart.planPieces().map((p) => p.state)).toEqual(['takeoff', 'inside', 'low']);
    expect(chart.planPieces()[0].d.startsWith('M0.0,')).toBe(true);
  });

  it('closes the ground and the corridor band', () => {
    expect(chart.groundArea().endsWith(`L0,${PROFILE_H}Z`)).toBe(true);
    expect(chart.band().endsWith('Z')).toBe(true);
    expect(chart.line('floorMslM').split('L')).toHaveLength(4);
  });

  it('marks the take-off zone and kilometres', () => {
    const zones = chart.takeoffZones();
    expect(zones).toHaveLength(1);
    expect(zones[0].x).toBe(0);
    expect(zones[0].w).toBeCloseTo(PROFILE_W / 3, 6);
    expect(chart.ticks().map((t) => t.label)).toEqual([
      '0 km',
      '0.5 km',
      '1 km',
      '1.5 km',
      '2 km',
      '2.5 km',
      '3 km',
    ]);
  });
});
