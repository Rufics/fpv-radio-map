import { Component, DestroyRef, OnInit, inject, input, signal } from '@angular/core';
import { ApiError, MissionStore, ModuleUiLoader, provideMapView } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { DesktopLayoutComponent } from './desktop-layout.component';
import { PhoneLayoutComponent } from './phone-layout.component';

/** The mission workspace: layout A on laptop/desktop, layout C on phones. */
@Component({
  selector: 'fpv-workspace-page',
  imports: [DesktopLayoutComponent, PhoneLayoutComponent],
  providers: [provideMapView()],
  template: `
    @if (loadError(); as e) {
      <div class="error" role="alert">
        <h1>Mission could not be opened</h1>
        <p>{{ e }}</p>
        <a class="rf-btn rf-btn--secondary" href="/">Start a new mission</a>
      </div>
    } @else if (phone()) {
      <fpv-phone-layout />
    } @else {
      <fpv-desktop-layout />
    }
  `,
  styles: `
    :host { display: block; height: 100%; }
    .error { max-width: 480px; margin: 80px auto; padding: 24px; display: flex; flex-direction: column; gap: 12px; }
    h1 { margin: 0; font-size: 22px; }
    p { color: var(--rf-text-3); }
  `,
})
export class WorkspacePage implements OnInit {
  /** Route parameter (mission/:id) */
  readonly id = input<string>();
  private readonly data = inject(FpvDataService);
  private readonly store = inject(MissionStore);
  private readonly ws = inject(FpvWorkspace);
  private readonly loader = inject(ModuleUiLoader);
  protected readonly phone = signal(false);
  protected readonly loadError = signal<string | null>(null);

  constructor() {
    const mq = window.matchMedia('(max-width: 767px)');
    this.phone.set(mq.matches);
    const on = (e: MediaQueryListEvent) => this.phone.set(e.matches);
    mq.addEventListener('change', on);
    inject(DestroyRef).onDestroy(() => mq.removeEventListener('change', on));
  }

  ngOnInit(): void {
    this.init().catch((e) => this.loadError.set(ApiError.from(e).message));
  }

  private async init(): Promise<void> {
    await this.data.load();
    if (this.loader.status().length === 0) void this.loader.loadAll(this.data.modules());
    const id = this.id();
    if (id && this.store.missionId() !== id) {
      try {
        this.ws.why.set(null);
        await this.store.load(id);
      } catch (e) {
        const err = ApiError.from(e);
        this.loadError.set(err.status === 401 || err.status === 403 ? 'This browser has no session for the mission. Open the shared link again and enter the PIN.' : err.message);
      }
    }
  }
}
