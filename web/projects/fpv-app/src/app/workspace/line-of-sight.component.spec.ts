import { describe, expect, it } from 'vitest';
import { SightReadout } from './line-of-sight.component';
import { MiniMapPainter } from './coverage-mini-map.component';
import { CoverageProbe } from '@rfmap/ui-kit';

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
