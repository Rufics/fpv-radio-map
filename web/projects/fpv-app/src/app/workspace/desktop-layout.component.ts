import { Component, inject } from '@angular/core';
import { RouterLink } from '@angular/router';
import { PillComponent, SlotHostComponent } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { PlanPanelComponent } from '../panels/plan-panel.component';
import { RadioSetupComponent } from '../panels/radio-setup.component';
import { ShareDialogComponent } from '../panels/share-dialog.component';
import { WeatherBriefingComponent } from '../panels/weather-briefing.component';
import { WhyDrawerComponent } from '../panels/why-drawer.component';
import { MapStageComponent } from './map-stage.component';

/** Layout A (laptop/desktop): header, 380 px side panel, map, bottom drawer. */
@Component({
  selector: 'fpv-desktop-layout',
  imports: [PillComponent, SlotHostComponent, PlanPanelComponent, MapStageComponent, WhyDrawerComponent, ShareDialogComponent, RadioSetupComponent, WeatherBriefingComponent, RouterLink],
  template: `
    <header class="bar">
      @if (ws.mission(); as m) {
        <b class="name">{{ m.name }}</b>
        <span class="time">{{ ws.timeText() }}</span>
      } @else {
        <b class="name">FPV Radio Map</b>
      }
      <div class="right">
        @if (!ws.store.preview()) {
          @for (p of ws.pills(); track p.key) {
            <rf-pill [tone]="p.tone" [attr.title]="p.title">{{ p.text }}</rf-pill>
          }
        }
        <rf-slot-host name="header.actions" />
        @if (ws.mission()) {
          <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="ws.shareOpen.set(true)" [disabled]="!ws.store.canEdit()">Share</button>
        }
        <a class="rf-btn rf-btn--sm rf-btn--muted" routerLink="/admin/library">Library</a>
        <a class="rf-btn rf-btn--sm rf-btn--muted" routerLink="/admin/modules">Admin</a>
      </div>
    </header>
    <div class="body">
      <aside class="side" aria-label="Mission planning">
        <fpv-plan-panel />
      </aside>
      <main class="main">
        <fpv-map-stage class="stage" [fitPadding]="padding" [drawerHeight]="340" />
        @if (ws.why()) {
          <fpv-why-drawer class="drawer" />
        }
        @if (ws.shareOpen()) {
          <fpv-share-dialog />
        }
        @if (ws.radioSetup()) {
          <fpv-radio-setup />
        }
        @if (ws.briefingOpen() && ws.mission()) {
          <fpv-weather-briefing />
        }
      </main>
    </div>
  `,
  styles: `
    :host { display: flex; flex-direction: column; height: 100%; }
    .bar { height: 56px; flex: none; display: flex; align-items: center; gap: 12px; padding: 0 16px; background: var(--rf-surface-1); border-bottom: 1px solid var(--rf-border); }
    .name { font-size: 16px; white-space: nowrap; }
    .time { font-size: 12px; color: var(--rf-text-2); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .right { margin-left: auto; display: flex; gap: 8px; align-items: center; }
    .right rf-slot-host { display: contents; }
    .body { flex: 1; min-height: 0; display: flex; }
    .side { width: 380px; flex: none; padding: 16px; background: var(--rf-surface-2); border-right: 1px solid var(--rf-border); overflow-y: auto; }
    .main { flex: 1; min-width: 0; position: relative; overflow: hidden; }
    .stage { position: absolute; inset: 0; }
    .drawer { position: absolute; left: 0; right: 0; bottom: 0; z-index: 10; }
  `,
})
export class DesktopLayoutComponent {
  protected readonly ws = inject(FpvWorkspace);
  protected readonly padding = { top: 70, bottom: 70, left: 80, right: 300 };
}
