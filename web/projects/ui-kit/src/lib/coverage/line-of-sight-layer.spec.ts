import { describe, expect, it } from 'vitest';
import { LineOfSightLayer, SIGHT_COLORS } from './line-of-sight-layer';
import { CoverageReliefLayer } from './coverage-relief-layer';

describe('LineOfSightLayer', () => {
  it('reads a margin as a tone', () => {
    expect([null, undefined, Number.NaN].map((m) => LineOfSightLayer.tone(m))).toEqual([
      'unknown',
      'unknown',
      'unknown',
    ]);
    expect([-0.5, 0, 9.9, 10].map((m) => LineOfSightLayer.tone(m))).toEqual([
      'blocked',
      'warn',
      'warn',
      'ok',
    ]);
  });

  it('draws the line, the end point and the obstruction', () => {
    const fc = LineOfSightLayer.features({
      from: { lat: 48, lon: 35 },
      to: { lat: 48.1, lon: 35.1 },
      tone: 'warn',
      obstruction: { lat: 48.05, lon: 35.05 },
    });
    expect(fc.features.map((f) => [f.properties?.['kind'], f.geometry.type])).toEqual([
      ['line', 'LineString'],
      ['end', 'Point'],
      ['obstruction', 'Point'],
    ]);
    expect(fc.features[0].properties?.['color']).toBe(SIGHT_COLORS.warn);
    expect(fc.features[2].properties?.['color']).toBe(SIGHT_COLORS.blocked);
  });

  it('draws nothing for no sight', () => {
    expect(LineOfSightLayer.features(null).features).toEqual([]);
  });
});

describe('CoverageReliefLayer', () => {
  it('prefers the pre-rendered hillshade, falls back to the DEM, and needs one of them', () => {
    expect(CoverageReliefLayer.fromConfig({})).toBeNull();
    const dem = CoverageReliefLayer.fromConfig({ dem: { pmtiles: '/tiles/terrain.pmtiles' } })!;
    expect(dem.styleLayerIds()).toEqual(['coverage-relief']);
    expect(dem.drawGroup).toBe('base');
    expect(dem.internal).toBe(true);
    const raster = CoverageReliefLayer.fromConfig({
      hillshade: { pmtiles: '/tiles/hillshade.pmtiles' },
      dem: { pmtiles: '/tiles/terrain.pmtiles' },
    })!;
    expect(raster['layers']()[0].type).toBe('raster');
  });
});
