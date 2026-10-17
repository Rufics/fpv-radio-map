/** Draws 32 random bits into `word`; the default is the browser's CSPRNG. */
export type RandomWord = (word: Uint32Array<ArrayBuffer>) => Uint32Array<ArrayBuffer>;

const cryptoWord: RandomWord = (word) => crypto.getRandomValues(word);

/**
 * A 6-digit PIN, uniform over 000000–999999. It takes the top 20 random bits (0–1 048 575) and
 * draws again when they exceed 999 999; a plain `% 1_000_000` of 32 bits would favour the low PINs.
 */
export function randomPin(random: RandomWord = cryptoWord): string {
  const word = new Uint32Array(1);
  let n: number;
  do {
    n = random(word)[0] >>> 12;
  } while (n >= 1_000_000);
  return String(n).padStart(6, '0');
}
