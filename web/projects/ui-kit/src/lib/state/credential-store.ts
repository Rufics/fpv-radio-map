import { Injectable, signal } from '@angular/core';

/** Browser storage that never throws (private mode, blocked storage). */
class SafeStorage {
  constructor(private readonly pick: () => Storage) {}
  get(key: string): string | null {
    try {
      return this.pick().getItem(key);
    } catch {
      return null;
    }
  }
  set(key: string, value: string | null): void {
    try {
      if (value === null) this.pick().removeItem(key);
      else this.pick().setItem(key, value);
    } catch {
      /* storage unavailable: keep in memory only */
    }
  }
}

/**
 * Sessions, owner keys and the admin token.
 * Owner keys live in localStorage (the Share dialog offers a download, as designed);
 * sessions and the admin token only for the tab.
 */
@Injectable({ providedIn: 'root' })
export class CredentialStore {
  private readonly local = new SafeStorage(() => localStorage);
  private readonly tab = new SafeStorage(() => sessionStorage);
  private readonly memory = new Map<string, string>();
  private readonly adminTokenState = signal<string | null>(this.read(this.tab, 'rfmap.admin'));
  /** The admin token as a signal, for UI that switches between read-only and editable. */
  readonly adminTokenSignal = this.adminTokenState.asReadonly();

  private read(store: SafeStorage, key: string): string | null {
    return store.get(key) ?? this.memory.get(key) ?? null;
  }
  private write(store: SafeStorage, key: string, value: string | null): void {
    if (value === null) this.memory.delete(key);
    else this.memory.set(key, value);
    store.set(key, value);
  }

  session(missionId: string | null): string | null {
    return missionId ? this.read(this.tab, `rfmap.session.${missionId}`) : null;
  }
  setSession(missionId: string, session: string | null): void {
    this.write(this.tab, `rfmap.session.${missionId}`, session);
  }

  ownerKey(missionId: string | null): string | null {
    return missionId ? this.read(this.local, `rfmap.owner.${missionId}`) : null;
  }
  setOwnerKey(missionId: string, key: string | null): void {
    this.write(this.local, `rfmap.owner.${missionId}`, key);
  }

  adminToken(): string | null {
    return this.read(this.tab, 'rfmap.admin');
  }
  setAdminToken(token: string | null): void {
    this.write(this.tab, 'rfmap.admin', token);
    this.adminTokenState.set(token);
  }

  lastMissionId(): string | null {
    return this.read(this.local, 'rfmap.lastMission');
  }
  setLastMissionId(id: string | null): void {
    this.write(this.local, 'rfmap.lastMission', id);
  }
}
