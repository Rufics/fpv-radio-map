import { InjectionToken } from '@angular/core';
import { OfflineAreaRequest, OfflineRegion } from '../models/api.models';

export type SavedPackageKind = 'regions' | 'areas';

/** A package kept on this device: what the catalog said of it when it was saved. */
export interface SavedPackage {
  /** "<kind>/<id>", one per region or drawn area */
  key: string;
  kind: SavedPackageKind;
  /** The catalog entry it was saved from (its URLs are the server's). */
  region: OfflineRegion;
  savedAt: string;
  /** A drawn area: what was asked for, sent again to learn whether a newer package exists. */
  request?: OfflineAreaRequest;
}

/** Where saved packages live in the browser. Records and zips apart, so listing reads no zip. */
export interface OfflineStorage {
  /** false when packages last only while the page is open (no IndexedDB in this browser). */
  readonly persistent: boolean;
  list(): Promise<SavedPackage[]>;
  zip(key: string): Promise<Blob | null>;
  /** Replaces the record and zip of `saved.key` together. */
  put(saved: SavedPackage, zip: Blob): Promise<void>;
  remove(key: string): Promise<void>;
}

/** The kept packages in IndexedDB: Blobs, which browsers keep on disk. */
export class IndexedDbOfflineStorage implements OfflineStorage {
  static readonly database = 'rfmap-offline';
  readonly persistent = true;
  private opened?: Promise<IDBDatabase>;

  constructor(private readonly idb: IDBFactory) {}

  private db(): Promise<IDBDatabase> {
    this.opened ??= new Promise((resolve, reject) => {
      const req = this.idb.open(IndexedDbOfflineStorage.database, 1);
      req.onupgradeneeded = () => {
        req.result.createObjectStore('packages', { keyPath: 'key' });
        req.result.createObjectStore('zips');
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error ?? new Error('IndexedDB cannot be opened'));
    });
    return this.opened;
  }

  private async run<T>(stores: string[], mode: IDBTransactionMode, work: (tx: IDBTransaction) => IDBRequest<T> | void): Promise<T> {
    const tx = (await this.db()).transaction(stores, mode);
    const req = work(tx);
    return new Promise<T>((resolve, reject) => {
      tx.oncomplete = () => resolve(req ? req.result : (undefined as T));
      tx.onerror = () => reject(tx.error ?? new Error('IndexedDB transaction failed'));
      tx.onabort = () => reject(tx.error ?? new Error('IndexedDB transaction aborted, the device may be out of space'));
    });
  }

  list(): Promise<SavedPackage[]> {
    return this.run<SavedPackage[]>(['packages'], 'readonly', (tx) => tx.objectStore('packages').getAll());
  }

  async zip(key: string): Promise<Blob | null> {
    const blob = await this.run<Blob | undefined>(['zips'], 'readonly', (tx) => tx.objectStore('zips').get(key));
    return blob ?? null;
  }

  put(saved: SavedPackage, zip: Blob): Promise<void> {
    return this.run<void>(['packages', 'zips'], 'readwrite', (tx) => {
      tx.objectStore('zips').put(zip, saved.key);
      tx.objectStore('packages').put(saved);
    });
  }

  remove(key: string): Promise<void> {
    return this.run<void>(['packages', 'zips'], 'readwrite', (tx) => {
      tx.objectStore('zips').delete(key);
      tx.objectStore('packages').delete(key);
    });
  }
}

/** Packages kept for this page only: tests, and browsers without IndexedDB. */
export class MemoryOfflineStorage implements OfflineStorage {
  private readonly records = new Map<string, { saved: SavedPackage; zip: Blob }>();

  constructor(readonly persistent = false) {}

  async list(): Promise<SavedPackage[]> {
    return [...this.records.values()].map((r) => r.saved);
  }

  async zip(key: string): Promise<Blob | null> {
    return this.records.get(key)?.zip ?? null;
  }

  async put(saved: SavedPackage, zip: Blob): Promise<void> {
    this.records.set(saved.key, { saved, zip });
  }

  async remove(key: string): Promise<void> {
    this.records.delete(key);
  }
}

/** The storage of saved packages: IndexedDB where the browser has it. */
export const OFFLINE_STORAGE = new InjectionToken<OfflineStorage>('OFFLINE_STORAGE', {
  factory: () =>
    typeof indexedDB === 'undefined' ? new MemoryOfflineStorage() : new IndexedDbOfflineStorage(indexedDB),
});

/** How packages are fetched: the browser's fetch, which streams; the dev mock serves its own. */
export const OFFLINE_FETCH = new InjectionToken<typeof fetch>('OFFLINE_FETCH', {
  factory: () => (input, init) => fetch(input, init),
});
