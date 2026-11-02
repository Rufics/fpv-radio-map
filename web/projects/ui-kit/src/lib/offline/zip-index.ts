/**
 * Reading files out of a region package kept as one zip (a Blob) without unpacking it. The
 * server stores every entry uncompressed (PMTiles and PNG are compressed already), so an entry
 * is a byte range of the zip and a tile is read with one Blob slice.
 */

/** One stored file of the zip. */
export interface ZipEntry {
  readonly name: string;
  readonly size: number;
  /** 0 = stored; anything else cannot be read as a range. */
  readonly method: number;
  /** Where the entry's local header starts. */
  readonly headerOffset: number;
}

const EOCD = 0x06054b50;
const ZIP64_LOCATOR = 0x07064b50;
const ZIP64_EOCD = 0x06064b50;
const CENTRAL = 0x02014b50;
const LOCAL = 0x04034b50;
const MAX16 = 0xffff;
const MAX32 = 0xffffffff;

function uint64(v: DataView, at: number): number {
  return v.getUint32(at + 4, true) * 0x1_0000_0000 + v.getUint32(at, true);
}

async function view(blob: Blob, start: number, end: number): Promise<DataView> {
  return new DataView(await blob.slice(start, end).arrayBuffer());
}

/** The central directory of a zip: where each entry is. */
export class ZipIndex {
  private readonly dataOffsets = new Map<string, Promise<number>>();

  private constructor(
    private readonly blob: Blob,
    private readonly entries: ReadonlyMap<string, ZipEntry>,
  ) {}

  /** Reads the directory at the end of `blob`; throws when it is not a zip. */
  static async read(blob: Blob): Promise<ZipIndex> {
    const tailStart = Math.max(0, blob.size - (MAX16 + 22 + 20));
    const tail = await view(blob, tailStart, blob.size);
    let eocd = -1;
    for (let i = tail.byteLength - 22; i >= 0; i--) {
      if (tail.getUint32(i, true) === EOCD) {
        eocd = i;
        break;
      }
    }
    if (eocd < 0) throw new Error('not a zip: no end of central directory');
    let count = tail.getUint16(eocd + 10, true);
    let size = tail.getUint32(eocd + 12, true);
    let offset = tail.getUint32(eocd + 16, true);
    if (count === MAX16 || size === MAX32 || offset === MAX32) {
      const locator = eocd - 20;
      if (locator < 0 || tail.getUint32(locator, true) !== ZIP64_LOCATOR) throw new Error('zip64 locator missing');
      const at = uint64(tail, locator + 8);
      const z = await view(blob, at, at + 56);
      if (z.getUint32(0, true) !== ZIP64_EOCD) throw new Error('zip64 end of central directory missing');
      count = uint64(z, 32);
      size = uint64(z, 40);
      offset = uint64(z, 48);
    }
    return new ZipIndex(blob, ZipIndex.parseDirectory(await view(blob, offset, offset + size), count));
  }

  private static parseDirectory(d: DataView, count: number): Map<string, ZipEntry> {
    const out = new Map<string, ZipEntry>();
    const decoder = new TextDecoder();
    let p = 0;
    for (let n = 0; n < count; n++) {
      if (d.getUint32(p, true) !== CENTRAL) throw new Error('damaged central directory');
      const method = d.getUint16(p + 10, true);
      const nameLength = d.getUint16(p + 28, true);
      const extraLength = d.getUint16(p + 30, true);
      const commentLength = d.getUint16(p + 32, true);
      const name = decoder.decode(new Uint8Array(d.buffer, d.byteOffset + p + 46, nameLength));
      const [size, compressed, headerOffset] = ZipIndex.zip64(
        d,
        p + 46 + nameLength,
        extraLength,
        [d.getUint32(p + 24, true), d.getUint32(p + 20, true), d.getUint32(p + 42, true)],
      );
      if (!name.endsWith('/')) out.set(name, { name, size: method === 0 ? size : compressed, method, headerOffset });
      p += 46 + nameLength + extraLength + commentLength;
    }
    return out;
  }

  /** Size, compressed size and header offset, with those that did not fit in 32 bits read
   * from the zip64 extra field (id 1), which holds them in this order. */
  private static zip64(d: DataView, start: number, length: number, values: [number, number, number]): [number, number, number] {
    const out: [number, number, number] = [...values];
    for (let e = start; e + 4 <= start + length; e += 4 + d.getUint16(e + 2, true)) {
      if (d.getUint16(e, true) !== 0x0001) continue;
      let q = e + 4;
      for (let i = 0; i < 3; i++) {
        if (out[i] !== MAX32) continue;
        out[i] = uint64(d, q);
        q += 8;
      }
    }
    return out;
  }

  names(): string[] {
    return [...this.entries.keys()];
  }

  has(name: string): boolean {
    return this.entries.has(name);
  }

  entry(name: string): ZipEntry | undefined {
    return this.entries.get(name);
  }

  /** `length` bytes of entry `name` from `offset` (default: all of it). */
  async bytes(name: string, offset = 0, length?: number): Promise<ArrayBuffer> {
    const e = this.entries.get(name);
    if (!e) throw new Error(`${name} is not in the package`);
    if (e.method !== 0) throw new Error(`${name} is compressed in the zip; only stored entries are read`);
    const start = (await this.dataOffset(e)) + offset;
    const end = start + Math.max(0, Math.min(length ?? e.size - offset, e.size - offset));
    return this.blob.slice(start, end).arrayBuffer();
  }

  async text(name: string): Promise<string> {
    return new TextDecoder().decode(await this.bytes(name));
  }

  /** Where the bytes of `e` start: after its local header, whose fields may differ in length. */
  private dataOffset(e: ZipEntry): Promise<number> {
    let known = this.dataOffsets.get(e.name);
    if (!known) {
      known = view(this.blob, e.headerOffset, e.headerOffset + 30).then((h) => {
        if (h.getUint32(0, true) !== LOCAL) throw new Error(`damaged local header of ${e.name}`);
        return e.headerOffset + 30 + h.getUint16(26, true) + h.getUint16(28, true);
      });
      this.dataOffsets.set(e.name, known);
    }
    return known;
  }
}

/**
 * Writes a zip of stored entries, as the server writes packages. Small and in memory: the dev
 * mock serves its packages with it, and tests make zips to read.
 */
export class StoredZipWriter {
  private readonly parts: Uint8Array[] = [];
  private readonly central: Uint8Array[] = [];
  private offset = 0;
  private count = 0;

  add(name: string, content: Uint8Array | string): this {
    const data = typeof content === 'string' ? new TextEncoder().encode(content) : content;
    const nameBytes = new TextEncoder().encode(name);
    const crc = StoredZipWriter.crc32(data);
    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, LOCAL, true);
    local.setUint16(4, 20, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, data.length, true);
    local.setUint32(22, data.length, true);
    local.setUint16(26, nameBytes.length, true);
    const entry = new DataView(new ArrayBuffer(46));
    entry.setUint32(0, CENTRAL, true);
    entry.setUint16(4, 20, true);
    entry.setUint16(6, 20, true);
    entry.setUint32(16, crc, true);
    entry.setUint32(20, data.length, true);
    entry.setUint32(24, data.length, true);
    entry.setUint16(28, nameBytes.length, true);
    entry.setUint32(42, this.offset, true);
    this.parts.push(new Uint8Array(local.buffer), nameBytes, data);
    this.central.push(new Uint8Array(entry.buffer), nameBytes);
    this.offset += 30 + nameBytes.length + data.length;
    this.count++;
    return this;
  }

  bytes(): Uint8Array {
    const directorySize = this.central.reduce((n, p) => n + p.length, 0);
    const end = new DataView(new ArrayBuffer(22));
    end.setUint32(0, EOCD, true);
    end.setUint16(8, this.count, true);
    end.setUint16(10, this.count, true);
    end.setUint32(12, directorySize, true);
    end.setUint32(16, this.offset, true);
    const all = [...this.parts, ...this.central, new Uint8Array(end.buffer)];
    const out = new Uint8Array(all.reduce((n, p) => n + p.length, 0));
    let at = 0;
    for (const p of all) {
      out.set(p, at);
      at += p.length;
    }
    return out;
  }

  private static crcTable?: Uint32Array;

  private static crc32(data: Uint8Array): number {
    const table = (StoredZipWriter.crcTable ??= Uint32Array.from({ length: 256 }, (_, n) => {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      return c >>> 0;
    }));
    let crc = 0xffffffff;
    for (const b of data) crc = table[(crc ^ b) & 0xff] ^ (crc >>> 8);
    return (crc ^ 0xffffffff) >>> 0;
  }
}
