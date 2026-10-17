/**
 * Signal scale: strong → at threshold. #F9E46B · #F28D3C · #C9417A · #6A2C8F.
 * Steps differ in lightness so the scale reads without colour vision.
 * Below threshold (margin < 0 dB) or no data: transparent, the map shows through.
 */
export const HEAT_STOPS: readonly { db: number; rgb: [number, number, number] }[] = [
  { db: 0, rgb: [0x6a, 0x2c, 0x8f] },
  { db: 10, rgb: [0xc9, 0x41, 0x7a] },
  { db: 20, rgb: [0xf2, 0x8d, 0x3c] },
  { db: 30, rgb: [0xf9, 0xe4, 0x6b] },
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
    if (m >= stops[stops.length - 1].db) return [...stops[stops.length - 1].rgb, this.alpha];
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
    return `linear-gradient(90deg, ${[...HEAT_STOPS].reverse().map((s) => `rgb(${s.rgb.join(',')})`).join(', ')})`;
  }
}
