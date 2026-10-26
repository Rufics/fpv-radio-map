/**
 * Signal scale (viridis): strong → at threshold. #FDE725 · #35B779 · #31688E · #440154.
 * Perceptually uniform, its lightness rises with the margin, so it reads without colour vision,
 * and none of its hues is a route status colour (teal ok, amber warn, red blocked).
 * Below threshold (margin < 0 dB) or no data: transparent, the map shows through.
 */
export const HEAT_STOPS: readonly { db: number; rgb: [number, number, number] }[] = [
  { db: 0, rgb: [0x44, 0x01, 0x54] },
  { db: 10, rgb: [0x31, 0x68, 0x8e] },
  { db: 20, rgb: [0x35, 0xb7, 0x79] },
  { db: 30, rgb: [0xfd, 0xe7, 0x25] },
];

export type Rgba = [number, number, number, number];

export class HeatScale {
  constructor(
    readonly thresholdDb = 0,
    readonly alpha = 255,
  ) {}

  /** Colour of one cell, or null for transparent. */
  color(marginDb: number | null | undefined): Rgba | null {
    if (marginDb === null || marginDb === undefined || Number.isNaN(marginDb)) return null;
    const m = marginDb - this.thresholdDb;
    if (m < 0) return null;
    const stops = HEAT_STOPS;
    const top = stops.at(-1)!;
    if (m >= top.db) return [...top.rgb, this.alpha];
    for (let i = 0; i < stops.length - 1; i++) {
      const a = stops[i];
      const b = stops[i + 1];
      if (m >= a.db && m <= b.db) {
        const t = (m - a.db) / (b.db - a.db);
        return [
          Math.round(a.rgb[0] + (b.rgb[0] - a.rgb[0]) * t),
          Math.round(a.rgb[1] + (b.rgb[1] - a.rgb[1]) * t),
          Math.round(a.rgb[2] + (b.rgb[2] - a.rgb[2]) * t),
          this.alpha,
        ];
      }
    }
    return null;
  }

  /** Fill an RGBA buffer (row-major, north to south) from a margin grid. */
  paint(margins: readonly (number | null)[], target: Uint8ClampedArray): void {
    for (let i = 0; i < margins.length; i++) {
      const c = this.color(margins[i]);
      const o = i * 4;
      if (c) {
        target[o] = c[0];
        target[o + 1] = c[1];
        target[o + 2] = c[2];
        target[o + 3] = c[3];
      } else {
        target[o + 3] = 0;
      }
    }
  }

  static cssGradient(): string {
    const colors = [...HEAT_STOPS].reverse().map((s) => HeatScale.cssRgb(s.rgb));
    return `linear-gradient(90deg, ${colors.join(', ')})`;
  }

  private static cssRgb(rgb: readonly number[]): string {
    return `rgb(${rgb.join(',')})`;
  }
}
