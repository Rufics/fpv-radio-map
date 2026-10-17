/**
 * The only place where numbers are rounded for display.
 * Values arrive at full precision; storage and calculation keep it.
 *
 * Rules (Tokens.dc.html "Numbers"):
 *  - dB and dBm: 2 decimals
 *  - distance: 3 decimals in breakdowns, 1 in summaries (km)
 *  - heights: whole metres
 *  - minus sign is U+2212; the full value is available for hover (title) and exports.
 */
export const MINUS = '−';

export type RfNumberKind =
  /** dB, 2 decimals, no forced sign: "106.13 dB" */
  | 'db'
  /** dB, 2 decimals, explicit sign: "+6.01 dB" / "−3.74 dB" */
  | 'db+'
  /** dBm, 2 decimals: "−98.74 dBm" */
  | 'dbm'
  /** dBi antenna gain, 2 decimals */
  | 'dbi'
  /** metres in, km with 3 decimals out (breakdowns): "5.280 km" */
  | 'km3'
  /** metres in, km with 1 decimal out (summaries): "10.8 km" */
  | 'km1'
  /** heights: whole metres "622 m" */
  | 'm'
  /** heights with explicit sign: "+10 m" / "−100 m" */
  | 'm+'
  /** lengths that are not heights (Fresnel radius), 2 decimals: "20.79 m" */
  | 'm2'
  /** dimensionless, 2 decimals: "6.80" */
  | 'n2'
  /** fraction 0..1 to whole percent: "62 %" */
  | 'pct'
  /** frequency in Hz to MHz with no decimals unless needed */
  | 'mhz';

interface KindSpec {
  decimals: number;
  unit: string;
  scale: number;
  signed: boolean;
}

const SPECS: Record<RfNumberKind, KindSpec> = {
  db: { decimals: 2, unit: 'dB', scale: 1, signed: false },
  'db+': { decimals: 2, unit: 'dB', scale: 1, signed: true },
  dbm: { decimals: 2, unit: 'dBm', scale: 1, signed: false },
  dbi: { decimals: 2, unit: 'dBi', scale: 1, signed: false },
  km3: { decimals: 3, unit: 'km', scale: 1e-3, signed: false },
  km1: { decimals: 1, unit: 'km', scale: 1e-3, signed: false },
  m: { decimals: 0, unit: 'm', scale: 1, signed: false },
  'm+': { decimals: 0, unit: 'm', scale: 1, signed: true },
  m2: { decimals: 2, unit: 'm', scale: 1, signed: false },
  n2: { decimals: 2, unit: '', scale: 1, signed: false },
  pct: { decimals: 0, unit: '%', scale: 100, signed: false },
  mhz: { decimals: 0, unit: 'MHz', scale: 1e-6, signed: false },
};

export interface FormatOptions {
  /** Omit the unit (for table cells whose header carries it). */
  unit?: boolean;
}

export class RfFormat {
  static readonly MINUS = MINUS;

  /** Replace ASCII hyphen-minus in a number string with U+2212. */
  static minus(text: string): string {
    return text.replace(/^-/, MINUS).replace(/([\s(])-(?=\d)/g, `$1${MINUS}`);
  }

  /** Round for display. Returns an em dash for missing values. */
  static format(value: number | null | undefined, kind: RfNumberKind, opts: FormatOptions = {}): string {
    if (value === null || value === undefined || Number.isNaN(value)) return '—';
    const spec = SPECS[kind];
    const scaled = value * spec.scale;
    let text = RfFormat.fixed(scaled, spec.decimals);
    const isZero = Number(text) === 0;
    if (isZero) text = text.replace('-', '');
    if (spec.signed && !isZero && !text.startsWith('-')) text = '+' + text;
    text = RfFormat.minus(text);
    if (opts.unit === false || !spec.unit) return text;
    return `${text} ${spec.unit}`;
  }

  /** Full precision for hover titles and exports, still with U+2212 and the unit. */
  static full(value: number | null | undefined, kind: RfNumberKind): string {
    if (value === null || value === undefined || Number.isNaN(value)) return '';
    const spec = SPECS[kind];
    const scaled = value * spec.scale;
    // Avoid binary-float noise like 5.2803700000000001 while keeping every significant digit.
    let text = String(Number(scaled.toPrecision(15)));
    if (spec.signed && scaled > 0) text = '+' + text;
    text = RfFormat.minus(text);
    return spec.unit ? `${text} ${spec.unit}` : text;
  }

  /** Exact-value string for machine exports (KML/GPX): plain ASCII, full precision. */
  static exportNumber(value: number): string {
    return String(Number(value.toPrecision(15)));
  }

  /** toFixed with half-away-from-zero rounding on the decimal representation. */
  private static fixed(value: number, decimals: number): string {
    const factor = 10 ** decimals;
    const rounded = Math.sign(value) * Math.round(Math.abs(value) * factor + 1e-9) / factor;
    return rounded.toFixed(decimals);
  }
}
