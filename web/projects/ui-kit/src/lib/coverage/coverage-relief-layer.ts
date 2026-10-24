import type { LayerSpecification, SourceSpecification } from 'maplibre-gl';
import { TileSourceRef } from '../models/api.models';
import { StyleLayerSet } from '../layers/layer-definition';
import { demSource, rasterSource } from '../layers/standard-layers';

/**
 * Hill shading drawn right over the coverage heatmap, so the colour reads as a tint of the
 * ground and the ridges that shape the coverage stay visible through it. Not in the layer panel:
 * the map shows it while the heatmap is on and the Hillshade layer (which is drawn over the
 * heatmap too) is off, so the relief is shaded once, never twice.
 */
export class CoverageReliefLayer extends StyleLayerSet {
  readonly id = 'results-coverage-relief';
  readonly title = 'Relief over the coverage';
  readonly group = 'results' as const;
  override readonly drawGroup = 'base' as const;
  /** Right above the heatmap (order 900). */
  override readonly order = 901;
  override readonly internal = true;
  override readonly lazy = true;
  override readonly defaultVisible = false;

  constructor(
    private readonly ref: TileSourceRef,
    private readonly mode: 'raster' | 'dem',
  ) {
    super();
  }

  protected sources(): Record<string, SourceSpecification> {
    return {
      'rf-coverage-relief': this.mode === 'dem' ? demSource(this.ref) : rasterSource(this.ref),
    };
  }

  protected layers(): LayerSpecification[] {
    if (this.mode === 'raster') {
      return [
        {
          id: 'coverage-relief',
          type: 'raster',
          source: 'rf-coverage-relief',
          paint: { 'raster-opacity': 0.7 },
        },
      ];
    }
    return [
      {
        id: 'coverage-relief',
        type: 'hillshade',
        source: 'rf-coverage-relief',
        paint: {
          'hillshade-shadow-color': 'rgba(4, 7, 11, 0.65)',
          'hillshade-highlight-color': 'rgba(255, 255, 255, 0.14)',
          'hillshade-accent-color': 'rgba(4, 7, 11, 0.25)',
          'hillshade-exaggeration': 0.55,
        },
      },
    ];
  }

  /** The relief layer for a map config: the pre-rendered hillshade, else shading of the DEM; null without either. */
  static fromConfig(config: {
    hillshade?: TileSourceRef;
    dem?: TileSourceRef;
  }): CoverageReliefLayer | null {
    if (config.hillshade) return new CoverageReliefLayer(config.hillshade, 'raster');
    return config.dem ? new CoverageReliefLayer(config.dem, 'dem') : null;
  }
}
