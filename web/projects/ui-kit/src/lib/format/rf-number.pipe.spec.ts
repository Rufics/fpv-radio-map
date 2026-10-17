import { RfFormat, MINUS } from './rf-format';
import { RfNumberPipe } from './rf-number.pipe';

describe('RfFormat / rfNum pipe', () => {
  const pipe = new RfNumberPipe();

  it('rounds dB and dBm to 2 decimals with U+2212', () => {
    expect(pipe.transform(-98.74265, 'dbm')).toBe(`${MINUS}98.74 dBm`);
    expect(pipe.transform(-95, 'dbm')).toBe(`${MINUS}95.00 dBm`);
    expect(pipe.transform(106.12874, 'db')).toBe('106.13 dB');
    expect(pipe.transform(29.61391, 'db')).toBe('29.61 dB');
  });

  it('signs margins explicitly', () => {
    expect(pipe.transform(6.01041, 'db+')).toBe('+6.01 dB');
    expect(pipe.transform(-3.74265, 'db+')).toBe(`${MINUS}3.74 dB`);
    expect(pipe.transform(25.32235, 'db+')).toBe('+25.32 dB');
  });

  it('never shows a negative zero', () => {
    expect(pipe.transform(-0.001, 'db+')).toBe('0.00 dB');
    expect(pipe.transform(-0.2, 'm')).toBe('0 m');
  });

  it('formats km with 3 decimals in breakdowns and 1 in summaries', () => {
    expect(pipe.transform(5280.04, 'km3')).toBe('5.280 km');
    expect(pipe.transform(10845.3, 'km1')).toBe('10.8 km');
    expect(pipe.transform(5940, 'km1')).toBe('5.9 km');
  });

  it('shows heights in whole metres and other lengths with 2 decimals', () => {
    expect(pipe.transform(622.4, 'm')).toBe('622 m');
    expect(pipe.transform(-100.2, 'm+')).toBe(`${MINUS}100 m`);
    expect(pipe.transform(10.4, 'm+')).toBe('+10 m');
    expect(pipe.transform(20.7934, 'm2')).toBe('20.79 m');
    expect(pipe.transform(6.8013, 'n2')).toBe('6.80');
  });

  it('rounds half away from zero', () => {
    expect(RfFormat.format(0.125, 'db')).toBe('0.13 dB');
    expect(RfFormat.format(-0.125, 'db')).toBe(`${MINUS}0.13 dB`);
    expect(RfFormat.format(1.005, 'db')).toBe('1.01 dB');
  });

  it('can omit the unit', () => {
    expect(pipe.transform(-98.74265, 'dbm', 'nounit')).toBe(`${MINUS}98.74`);
  });

  it('gives the full value for hover', () => {
    expect(pipe.transform(-98.74265, 'dbm', 'full')).toBe(`${MINUS}98.74265 dBm`);
    expect(pipe.transform(5280.37, 'km3', 'full')).toBe('5.28037 km');
    expect(pipe.transform(6.01041, 'db+', 'full')).toBe('+6.01041 dB');
  });

  it('handles missing values', () => {
    expect(pipe.transform(undefined, 'db')).toBe('—');
    expect(pipe.transform(null, 'db', 'full')).toBe('');
    expect(pipe.transform(Number.NaN, 'dbm')).toBe('—');
  });

  it('exports plain ASCII full precision', () => {
    expect(RfFormat.exportNumber(-98.74265)).toBe('-98.74265');
    expect(RfFormat.exportNumber(0.1 + 0.2)).toBe('0.3');
  });
});
