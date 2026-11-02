import { OfflineRegion } from '../models/api.models';
import { Sha256 } from './sha256';

export interface DownloadProgress {
  received: number;
  total: number;
}

/** A download that is not the package the catalog describes. */
export class DamagedDownloadError extends Error {}

/**
 * Downloads a package zip and checks it against the catalog: its size and SHA-256. The bytes are
 * hashed as they arrive and gathered into Blobs of `partBytes`, which the browser may keep on
 * disk, so a package of gigabytes is never held as one buffer.
 */
export class PackageDownloader {
  static readonly partBytes = 64 * 1024 * 1024;

  constructor(private readonly fetchFn: typeof fetch) {}

  async download(region: OfflineRegion, onProgress: (p: DownloadProgress) => void, signal?: AbortSignal): Promise<Blob> {
    const res = await this.fetchFn(region.package, { signal });
    if (!res.ok) throw new Error(res.status === 404 ? 'This version was replaced; reload the list' : `The server answered ${res.status}`);
    const total = region.size;
    const hash = new Sha256();
    const parts: Blob[] = [];
    let pending: Uint8Array[] = [];
    let pendingBytes = 0;
    let received = 0;
    const flush = () => {
      if (pending.length) parts.push(new Blob(pending as BlobPart[]));
      pending = [];
      pendingBytes = 0;
    };
    const take = (chunk: Uint8Array) => {
      hash.update(chunk);
      pending.push(chunk);
      pendingBytes += chunk.length;
      received += chunk.length;
      if (pendingBytes >= PackageDownloader.partBytes) flush();
      onProgress({ received, total });
    };
    if (res.body) {
      const reader = res.body.getReader();
      for (let r = await reader.read(); !r.done; r = await reader.read()) take(r.value);
    } else {
      take(new Uint8Array(await res.arrayBuffer()));
    }
    flush();
    if (received !== total) {
      throw new DamagedDownloadError(`The download has ${received} bytes, the package ${total}; try again`);
    }
    if (hash.hex() !== region.sha256) throw new DamagedDownloadError('The download is damaged (its SHA-256 differs); try again');
    return new Blob(parts, { type: 'application/zip' });
  }
}
