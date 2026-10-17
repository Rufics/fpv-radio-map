import { RandomWord, randomPin } from './random-pin';

/** Feeds the given 32-bit words in order. */
function words(...values: number[]): RandomWord {
  let i = 0;
  return (word) => {
    word[0] = values[i++];
    return word;
  };
}

describe('randomPin', () => {
  it('uses the top 20 bits as the PIN', () => {
    expect(randomPin(words(0))).toBe('000000');
    expect(randomPin(words(42 << 12))).toBe('000042');
    expect(randomPin(words((999_999 << 12) | 0xfff))).toBe('999999');
  });

  it('draws again above 999 999 instead of wrapping', () => {
    const above = 1_000_000 * 4096;
    const max = 0xffff_ffff;
    expect(randomPin(words(above, max, 123_456 * 4096))).toBe('123456');
  });

  it('gives six digits from the browser generator', () => {
    for (let i = 0; i < 100; i++) expect(randomPin()).toMatch(/^\d{6}$/);
  });
});
