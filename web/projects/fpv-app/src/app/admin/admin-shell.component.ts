import { Component, inject, signal } from '@angular/core';
import { RouterLink, RouterLinkActive, RouterOutlet } from '@angular/router';
import { CredentialStore } from '@rfmap/ui-kit';

/** Admin pages: header with section nav and the admin token (kept for this tab only). */
@Component({
  selector: 'fpv-admin-shell',
  imports: [RouterOutlet, RouterLink, RouterLinkActive],
  template: `
    <header class="bar">
      <a routerLink="/" class="home"><strong>FPV Radio Map</strong></a><span class="sep">/</span>
      <nav aria-label="Admin sections">
        <a routerLink="modules" routerLinkActive="on" ariaCurrentWhenActive="page">Modules</a>
        <a routerLink="library" routerLinkActive="on" ariaCurrentWhenActive="page">Reference library</a>
        <a routerLink="sources" routerLinkActive="on" ariaCurrentWhenActive="page">Data sources</a>
      </nav>
      <span class="rf-badge rf-badge--admin">Admin</span>
      <span class="grow"></span>
      @if (token()) {
        <span class="rf-caption">Admin token set</span>
        <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" (click)="clear()">Forget token</button>
      } @else {
        <label class="tok"><span class="sr-only">Admin token</span>
          <input class="rf-input rf-mono" type="password" placeholder="Admin token" [value]="draft()" (input)="draft.set($any($event.target).value)" (keydown.enter)="save()" />
        </label>
        <button type="button" class="rf-btn rf-btn--dense rf-btn--primary" (click)="save()">Use token</button>
      }
    </header>
    <div class="content"><router-outlet /></div>
  `,
  styles: `
    :host { display: flex; flex-direction: column; height: 100%; }
    .bar { min-height: 56px; flex: none; display: flex; align-items: center; gap: 14px; padding: 0 20px; border-bottom: 1px solid var(--rf-border); background: var(--rf-surface-1); flex-wrap: wrap; }
    .home { color: var(--rf-text); text-decoration: none; }
    .sep { color: var(--rf-border-strong); }
    nav { display: flex; gap: 4px; }
    nav a { padding: 8px 12px; border-radius: 8px; color: var(--rf-text-2); text-decoration: none; font-weight: 600; }
    nav a.on { background: var(--rf-selected); color: var(--rf-text); }
    .grow { flex: 1; }
    .tok input { height: 32px; width: 180px; }
    .content { flex: 1; min-height: 0; overflow: auto; }
  `,
})
export class AdminShellComponent {
  private readonly creds = inject(CredentialStore);
  protected readonly token = signal(this.creds.adminToken());
  protected readonly draft = signal('');

  protected save(): void {
    const t = this.draft().trim();
    if (!t) return;
    this.creds.setAdminToken(t);
    this.token.set(t);
    this.draft.set('');
  }

  protected clear(): void {
    this.creds.setAdminToken(null);
    this.token.set(null);
  }
}
