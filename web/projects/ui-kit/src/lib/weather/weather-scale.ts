import { RfFormat } from '../format/rf-format';
import { WeatherReading } from './route-weather';

/** What the route is coloured by. */
export type WeatherMetric = 'headwind' | 'wind' | 'temperature' | 'rain';

/** How much a value matters to the flight. */
export type WeatherLevel = 'calm' | 'notable' | 'strong';

export interface WeatherLegendItem {
  color: string;
  label: string;
}

interface MetricSpec {
  title: string;
  /** Upper bounds of the first classes; the last class has none */
  bounds: number[];
  colors: string[];
  labels: string[];
  value: (r: WeatherReading) => number | undefined;
  format: (v: number) => string;
  /** How much each class matters to the flight */
  levels: WeatherLevel[];
}

/**
 * Classes and colours of weather values along the route. Colours come from the ui-kit tokens and
 * differ in lightness as well as hue, so the classes stay apart without colour vision.
 */
export class WeatherScale {
  static readonly METRICS: readonly WeatherMetric[] = ['headwind', 'wind', 'temperature', 'rain'];

  private static readonly SPECS: Record<WeatherMetric, MetricSpec> = {
    headwind: {
      title: 'Head / tail',
      // headwind is negative: strong headwind first
      bounds: [-6, -3, 0],
      colors: ['#FF6B5B', '#F5A524', '#A3AFBD', '#35D0BA'],
      labels: ['headwind > 6 m/s', 'headwind 3–6 m/s', 'light headwind', 'tailwind'],
      value: (r) => r.alongTrackMps,
      format: (v) => `${v < 0 ? 'head' : 'tail'} ${RfFormat.format(Math.abs(v), 'mps')}`,
      levels: ['strong', 'notable', 'calm', 'calm'],
    },
    wind: {
      title: 'Wind',
      bounds: [8, 12],
      colors: ['#7BE6D6', '#F5A524', '#FF6B5B'],
      labels: ['< 8 m/s', '8–12 m/s', '≥ 12 m/s'],
      value: (r) => r.windSpeedMps,
      format: (v) => RfFormat.format(v, 'mps'),
      levels: ['calm', 'notable', 'strong'],
    },
    temperature: {
      title: 'Temp',
      bounds: [0, 15, 25],
      colors: ['#B9CBFF', '#35D0BA', '#F7BC55', '#FF6B5B'],
      labels: ['< 0 °C', '0–15 °C', '15–25 °C', '≥ 25 °C'],
      value: (r) => r.temperatureC,
      format: (v) => RfFormat.format(v, 'degc'),
      levels: ['notable', 'calm', 'calm', 'notable'],
    },
    rain: {
      title: 'Rain',
      bounds: [0.1, 1],
      colors: ['#5D6B7A', '#9DB4FF', '#D9B8F5'],
      labels: ['dry', '0.1–1 mm/h', '≥ 1 mm/h'],
      value: (r) => r.precipitationMmPerHour,
      format: (v) => RfFormat.format(v, 'mmh'),
      levels: ['calm', 'notable', 'strong'],
    },
  };

  /** Colour used when a reading has no value for the metric. */
  static readonly NO_DATA = '#5D6B7A';

  static title(metric: WeatherMetric): string {
    return WeatherScale.SPECS[metric].title;
  }

  static value(metric: WeatherMetric, r: WeatherReading): number | undefined {
    return WeatherScale.SPECS[metric].value(r);
  }

  static format(metric: WeatherMetric, v: number | undefined): string {
    return v === undefined ? '—' : WeatherScale.SPECS[metric].format(v);
  }

  static color(metric: WeatherMetric, v: number | undefined): string {
    if (v === undefined || Number.isNaN(v)) return WeatherScale.NO_DATA;
    const spec = WeatherScale.SPECS[metric];
    return spec.colors[WeatherScale.classOf(spec, v)];
  }

  static level(metric: WeatherMetric, v: number | undefined): WeatherLevel {
    if (v === undefined || Number.isNaN(v)) return 'calm';
    const spec = WeatherScale.SPECS[metric];
    return spec.levels[WeatherScale.classOf(spec, v)];
  }

  static legend(metric: WeatherMetric): WeatherLegendItem[] {
    const spec = WeatherScale.SPECS[metric];
    return spec.colors.map((color, i) => ({ color, label: spec.labels[i] }));
  }

  private static classOf(spec: MetricSpec, v: number): number {
    const i = spec.bounds.findIndex((b) => v < b);
    return i < 0 ? spec.bounds.length : i;
  }
}
