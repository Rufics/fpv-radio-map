import { Injectable, computed, inject, signal } from '@angular/core';
import {
  DownloadProgress,
  MapSourcesConfig,
  OFFLINE_FETCH,
  OFFLINE_STORAGE,
  OfflineAreaRequest,
  OfflineMapSources,
  OfflineRegion,
  PackageDownloader,
  SavedPackage,
  SavedPackageKind,
} from '@rfmap/ui-kit';

/** An area this browser asked the server to build, kept so its build can be followed later. */
export interface DrawnArea {
  id: string;
  name: string;
  request: OfflineAreaRequest;
  askedAt: string;
}

/**
 * The packages saved on this device: saving one (download, check, keep), updating it when the
 * catalog has a newer version, removing it, and the one the map reads instead of the server's
 * tiles. Also the areas this browser drew, so their builds can be followed after a reload.
 */
@Injectable({ providedIn: 'root' })
export class OfflineLibrary {
  private static readonly activeKey = 'rfmap.offline.active';
  private static readonly drawnKey = 'rfmap.offline.areas';

  private readonly storage = inject(OFFLINE_STORAGE);
  private readonly downloader = new PackageDownloader(inject(OFFLINE_FETCH));
  private readonly aborts = new Map<string, AbortController>();

  /** Whether saved packages outlive the page (no IndexedDB: they do not). */
  readonly persistent = this.storage.persistent;
  readonly saved = signal<SavedPackage[]>([]);
  /** Downloads in progress, by key. */
  readonly saving = signal<Record<string, DownloadProgress>>({});
  /** Why the last save of a key failed. */
  readonly failures = signal<Record<string, string>>({});
  /** The saved package the map reads, by key. */
  readonly active = signal<string | null>(OfflineLibrary.read(OfflineLibrary.activeKey));
  readonly drawn = signal<DrawnArea[]>(OfflineLibrary.readList<DrawnArea>(OfflineLibrary.drawnKey));
  readonly savedBytes = computed(() => this.saved().reduce((n, s) => n + s.region.size, 0));
  /** Counts changes to what the map reads: another package chosen, or the chosen one saved anew or removed. */
  readonly mapRevision = signal(0);

  static key(kind: SavedPackageKind, id: string): string {
    return `${kind}/${id}`;
  }

  async refresh(): Promise<void> {
    const list = await this.storage.list();
    this.saved.set(list.sort((a, b) => a.region.name.localeCompare(b.region.name)));
    const active = this.active();
    if (active && !list.some((s) => s.key === active)) this.setActive(list[0]?.key ?? null);
  }

  find(key: string): SavedPackage | undefined {
    return this.saved().find((s) => s.key === key);
  }

  /** Whether the catalog's `current` is newer than the copy saved here. */
  updateAvailable(kind: SavedPackageKind, current: OfflineRegion): boolean {
    const saved = this.find(OfflineLibrary.key(kind, current.id));
    return !!saved && saved.region.version !== current.version;
  }

  /** Downloads `region`, checks it and keeps it; replaces an older saved version only once it is complete. */
  async save(kind: SavedPackageKind, region: OfflineRegion, request?: OfflineAreaRequest): Promise<void> {
    const key = OfflineLibrary.key(kind, region.id);
    if (this.aborts.has(key)) return;
    const abort = new AbortController();
    this.aborts.set(key, abort);
    this.failures.update(({ [key]: _, ...rest }) => rest);
    this.progress(key, { received: 0, total: region.size });
    try {
      await this.checkRoom(region.size);
      await navigator.storage?.persist?.().catch(() => false);
      const zip = await this.downloader.download(region, (p) => this.progress(key, p), abort.signal);
      await this.storage.put({ key, kind, region, savedAt: new Date().toISOString(), request }, zip);
      OfflineMapSources.close(key);
      if (this.active()) this.changedMap(key);
      else this.setActive(key);
      await this.refresh();
    } catch (e) {
      if (!abort.signal.aborted) this.failures.update((f) => ({ ...f, [key]: e instanceof Error ? e.message : String(e) }));
    } finally {
      this.aborts.delete(key);
      this.saving.update(({ [key]: _, ...rest }) => rest);
    }
  }

  cancel(key: string): void {
    this.aborts.get(key)?.abort();
  }

  async remove(key: string): Promise<void> {
    await this.storage.remove(key);
    OfflineMapSources.close(key);
    await this.refresh();
    this.changedMap(key);
  }

  setActive(key: string | null): void {
    if (key === this.active()) return;
    this.active.set(key);
    OfflineLibrary.write(OfflineLibrary.activeKey, key);
    this.mapRevision.update((n) => n + 1);
  }

  remember(area: DrawnArea): void {
    this.drawn.update((list) => [area, ...list.filter((a) => a.id !== area.id)]);
    OfflineLibrary.write(OfflineLibrary.drawnKey, JSON.stringify(this.drawn()));
  }

  forget(id: string): void {
    this.drawn.update((list) => list.filter((a) => a.id !== id));
    OfflineLibrary.write(OfflineLibrary.drawnKey, JSON.stringify(this.drawn()));
  }

  /**
   * The map configuration of the package the map reads, over `base` (the server's, or null
   * when the server cannot be reached); null when no package is chosen or it cannot be read.
   */
  async mapConfig(base: MapSourcesConfig | null): Promise<MapSourcesConfig | null> {
    const key = this.active();
    if (!key) return null;
    try {
      const saved = (await this.storage.list()).find((s) => s.key === key);
      const zip = saved ? await this.storage.zip(key) : null;
      return saved && zip ? await OfflineMapSources.open(saved, zip, base) : null;
    } catch {
      return null; // a damaged or missing package: the server's map instead
    }
  }

  /** A package changed: when it is the one the map reads, the map's configuration is built again. */
  private changedMap(key: string): void {
    if (key === this.active()) this.mapRevision.update((n) => n + 1);
  }

  private progress(key: string, p: DownloadProgress): void {
    this.saving.update((s) => ({ ...s, [key]: p }));
  }

  /** Throws when the browser says the package does not fit in what it lets this site keep. */
  private async checkRoom(bytes: number): Promise<void> {
    const estimate = await navigator.storage?.estimate?.().catch(() => undefined);
    if (!estimate?.quota) return;
    const free = estimate.quota - (estimate.usage ?? 0);
    if (bytes > free) {
      throw new Error(`Not enough room: the package needs ${Math.ceil(bytes / 1e6)} MB, this browser lets the app keep ${Math.floor(free / 1e6)} MB more`);
    }
  }

  private static read(name: string): string | null {
    try {
      return localStorage.getItem(name);
    } catch {
      return null;
    }
  }

  private static readList<T>(name: string): T[] {
    try {
      const list: unknown = JSON.parse(OfflineLibrary.read(name) ?? '[]');
      return Array.isArray(list) ? (list as T[]) : [];
    } catch {
      return [];
    }
  }

  private static write(name: string, value: string | null): void {
    try {
      if (value === null) localStorage.removeItem(name);
      else localStorage.setItem(name, value);
    } catch {
      // private browsing: kept for this page only
    }
  }
}
