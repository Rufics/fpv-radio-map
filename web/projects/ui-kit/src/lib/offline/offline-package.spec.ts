import { describe, expect, it } from 'vitest';
import { OfflineRegion } from '../models/api.models';
import { AreaShape } from './area-shape';
import { OfflineMapSources } from './offline-map';
import { MemoryOfflineStorage, SavedPackage } from './offline-storage';
import { DamagedDownloadError, PackageDownloader } from './package-downloader';
import { Sha256 } from './sha256';
import { StoredZipWriter, ZipIndex } from './zip-index';

const sample = () => Uint8Array.from({ length: 1_000_003 }, (_, i) => (i * 31) & 255);

function region(zip: Uint8Array, over: Partial<OfflineRegion> = {}): OfflineRegion {
  return {
    id: 'ridge',
    name: 'Ridge',
    bbox: [35.0, 48.0, 35.4, 48.4],
    maxzoom: 14,
    layers: ['basemap', 'terrain', 'contours'],
    size: zip.length,
    version: 'v'.repeat(64),
    sha256: Sha256.hex(zip),
    builtAt: '2026-11-02T10:00:00Z',
    preview: null,
    package: '/api/v1/regions/ridge/packages/v.zip',
    ...over,
  };
}

/** A fetch that answers with `bytes` in chunks of `chunk`. */
function serving(bytes: Uint8Array, chunk = 4096, status = 200): typeof fetch {
  return async () => {
    let at = 0;
    const body = new ReadableStream<Uint8Array>({
      pull(c) {
        if (at >= bytes.length) return c.close();
        c.enqueue(bytes.slice(at, at + chunk));
        at += chunk;
      },
    });
    return new Response(status === 200 ? body : null, { status });
  };
}

describe('Sha256', () => {
  it('matches the standard digests, however the data is cut', () => {
    expect(Sha256.hex('')).toBe('e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
    expect(Sha256.hex('abc')).toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    const data = sample();
    const expected = 'f80cf937bfbbf381219cb6daf5e662fb420258971913c415468d6b7dd9aba370';
    expect(Sha256.hex(data)).toBe(expected);
    const pieces = new Sha256();
    for (let at = 0, step = 1; at < data.length; at += step, step = (step * 7) % 997 + 1) {
      pieces.update(data.subarray(at, at + step));
    }
    expect(pieces.hex()).toBe(expected);
    expect(() => pieces.update(new Uint8Array(1))).toThrow();
  });
});

describe('ZipIndex', () => {
  it('reads each stored entry, and any range of it, out of the zip', async () => {
    const big = sample();
    const zip = new StoredZipWriter()
      .add('manifest.json', '{"format":"rfmap-region"}')
      .add('glyphs/Noto Sans Regular/0-255.pbf', 'latin')
      .add('tiles/basemap.pmtiles', big)
      .bytes();
    const index = await ZipIndex.read(new Blob([zip as BlobPart]));
    expect(index.names()).toEqual(['manifest.json', 'glyphs/Noto Sans Regular/0-255.pbf', 'tiles/basemap.pmtiles']);
    expect(await index.text('manifest.json')).toBe('{"format":"rfmap-region"}');
    expect(index.entry('tiles/basemap.pmtiles')?.size).toBe(big.length);
    expect(new Uint8Array(await index.bytes('tiles/basemap.pmtiles', 500_000, 10))).toEqual(big.slice(500_000, 500_010));
    expect(new Uint8Array(await index.bytes('tiles/basemap.pmtiles', big.length - 3, 100))).toEqual(big.slice(-3));
    await expect(index.bytes('tiles/satellite.pmtiles')).rejects.toThrow('not in the package');
  });

  it('refuses what is not a zip', async () => {
    await expect(ZipIndex.read(new Blob(['just text']))).rejects.toThrow('not a zip');
  });
});

describe('PackageDownloader', () => {
  it('keeps a download whose size and SHA-256 are the catalog’s, and reports its progress', async () => {
    const zip = new StoredZipWriter().add('tiles/basemap.pmtiles', sample()).bytes();
    const seen: number[] = [];
    const blob = await new PackageDownloader(serving(zip)).download(region(zip), (p) => seen.push(p.received));
    expect(blob.size).toBe(zip.length);
    expect(seen.at(-1)).toBe(zip.length);
    expect(seen.length).toBeGreaterThan(100);
    expect((await ZipIndex.read(blob)).has('tiles/basemap.pmtiles')).toBe(true);
  });

  it('refuses a short, damaged or replaced download', async () => {
    const zip = new StoredZipWriter().add('a', 'abc').bytes();
    const d = new PackageDownloader(serving(zip));
    await expect(d.download(region(zip, { size: zip.length + 1 }), () => undefined)).rejects.toBeInstanceOf(DamagedDownloadError);
    await expect(d.download(region(zip, { sha256: '0'.repeat(64) }), () => undefined)).rejects.toThrow('SHA-256');
    const gone = new PackageDownloader(serving(zip, 4096, 404));
    await expect(gone.download(region(zip), () => undefined)).rejects.toThrow('replaced');
  });

  it('stops reading a download that grows past the package size', async () => {
    const big = sample();
    const seen: number[] = [];
    const d = new PackageDownloader(serving(big, 65_536));
    await expect(d.download(region(big, { size: 100_000 }), (p) => seen.push(p.received))).rejects.toThrow('larger');
    expect(Math.max(...seen)).toBeLessThanOrEqual(100_000);
  });
});

describe('OfflineMapSources', () => {
  it('points the map layers the package has at the package, and keeps the rest of the server’s', async () => {
    const zip = new StoredZipWriter()
      .add('tiles/basemap.pmtiles', 'PMTiles')
      .add('tiles/terrain.pmtiles', 'PMTiles')
      .add('glyphs/Noto Sans Regular/0-255.pbf', 'latin')
      .bytes();
    const saved: SavedPackage = { key: 'regions/ridge', kind: 'regions', region: region(zip), savedAt: '' };
    const config = await OfflineMapSources.open(saved, new Blob([zip as BlobPart]), {
      basemap: { pmtiles: '/tiles/basemap.pmtiles', attribution: 'OSM' },
      contours: { pmtiles: '/tiles/contours.pmtiles', sourceLayer: 'contours' },
      weather: { tiles: ['/tiles/weather/{layer}/{time}/{z}/{x}/{y}.png'] },
    });
    expect(config.basemap).toEqual(expect.objectContaining({ pmtiles: `rfoffline/regions/ridge/${'v'.repeat(64)}/basemap`, attribution: 'OSM' }));
    expect(config.dem).toEqual(expect.objectContaining({ encoding: 'terrarium', pmtiles: expect.stringMatching(/\/terrain$/) }));
    expect(config.contours?.pmtiles).toBe('/tiles/contours.pmtiles'); // not in the package
    expect(config.weather?.tiles?.length).toBe(1);
    expect(config.glyphs).toBe('rfoffline://regions/ridge/glyphs/{fontstack}/{range}.pbf');
    expect(config.center).toEqual([35.2, 48.2]);
    OfflineMapSources.close(saved.key);
  });
});

describe('MemoryOfflineStorage', () => {
  it('keeps a record and its zip together', async () => {
    const store = new MemoryOfflineStorage();
    const saved: SavedPackage = { key: 'areas/area-1', kind: 'areas', region: region(new Uint8Array(1)), savedAt: 'now' };
    await store.put(saved, new Blob(['zip']));
    expect((await store.list()).map((s) => s.key)).toEqual(['areas/area-1']);
    expect(await (await store.zip('areas/area-1'))?.text()).toBe('zip');
    await store.remove('areas/area-1');
    expect(await store.list()).toEqual([]);
    expect(await store.zip('areas/area-1')).toBeNull();
  });
});

describe('AreaShape', () => {
  it('measures what is drawn as the server does', () => {
    const square = new AreaShape([
      { lat: 0, lon: 0 },
      { lat: 0, lon: 1 },
      { lat: 1, lon: 1 },
      { lat: 1, lon: 0 },
    ]);
    expect(square.areaKm2()).toBeCloseTo(12364, -1);
    expect(square.geometry()?.coordinates[0]).toEqual([[0, 0], [1, 0], [1, 1], [0, 1], [0, 0]]);
    expect(square.crossesItself()).toBe(false);
    expect(square.withoutLast().areaKm2()).toBeGreaterThan(0);
    expect(new AreaShape([{ lat: 0, lon: 0 }]).with({ lat: 1, lon: 1 }).geometry()).toBeNull();
    const bowTie = new AreaShape([
      { lat: 0, lon: 0 },
      { lat: 1, lon: 1 },
      { lat: 1, lon: 0 },
      { lat: 0, lon: 1 },
    ]);
    expect(bowTie.crossesItself()).toBe(true);
  });
});
