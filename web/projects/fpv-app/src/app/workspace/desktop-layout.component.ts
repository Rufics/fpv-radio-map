import { Component, DestroyRef, ElementRef, effect, inject, signal, viewChild } from '@angular/core';
import { RouterLink } from '@angular/router';
import { PillComponent, SegmentOption, SegmentedComponent, SlotHostComponent } from '@rfmap/ui-kit';
import { FlightReplay, WorkspaceMode } from '../core/flight-replay.service';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { PlanPanelComponent } from '../panels/plan-panel.component';
import { RadioSetupComponent } from '../panels/radio-setup.component';
import { ShareDialogComponent } from '../panels/share-dialog.component';
import { WeatherBriefingComponent } from '../panels/weather-briefing.component';
import { WhyDrawerComponent } from '../panels/why-drawer.component';
import { AltitudeDrawerComponent } from '../panels/altitude-drawer.component';
import { AltitudeView } from '../core/fpv-altitude';
import { FlightImportComponent } from '../replay/flight-import.component';
import { PlanFactPanelComponent } from '../replay/plan-fact-panel.component';
import { ReplayDrawerComponent } from '../replay/replay-drawer.component';
import { ReplayPanelComponent } from '../replay/replay-panel.component';
import { MapStageComponent } from './map-stage.component';

/** Layout A (laptop/desktop): header, 380 px side panel, map, bottom drawer. */
@Component({
  selector: 'fpv-desktop-layout',
  imports: [
    PillComponent,
    SegmentedComponent,
    SlotHostComponent,
    PlanPanelComponent,
    MapStageComponent,
    WhyDrawerComponent,
    AltitudeDrawerComponent,
    ShareDialogComponent,
    RadioSetupComponent,
    WeatherBriefingComponent,
    RouterLink,
    ReplayPanelComponent,
    PlanFactPanelComponent,
    ReplayDrawerComponent,
    FlightImportComponent,
  ],
  template: `
    <header class="bar">
      @if (ws.mission(); as m) {
        <b class="name">{{ m.name }}</b>
        <span class="time">{{ ws.timeText() }}</span>
      } @else {
        <b class="name">FPV Radio Map</b>
      }
      @if (ws.mission() && !ws.store.preview()) {
        <rf-segmented class="modes" label="Workspace" [options]="modes" [value]="replay.mode()" (valueChange)="setMode($event)" />
      }
      <div class="right">
        @if (replay.mode() !== 'plan') {
          <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="replay.importOpen.set(true)">Load log</button>
        } @else if (!ws.store.preview()) {
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
      <aside class="side" [attr.aria-label]="replay.mode() === 'plan' ? 'Mission planning' : 'Flight replay'">
        @switch (replay.mode()) {
          @case ('replay') {
            <fpv-replay-panel />
          }
          @case ('plan-fact') {
            <fpv-plan-fact-panel />
          }
          @default {
            <fpv-plan-panel />
          }
        }
      </aside>
      <main class="main">
        <fpv-map-stage class="stage" [fitPadding]="padding" [drawerHeight]="340" [style.--fpv-cover-bottom]="cover() + 'px'" />
        @if (replay.mode() !== 'plan' && replay.view()) {
          <fpv-replay-drawer class="drawer" #drawer />
        } @else if (ws.why()) {
          <fpv-why-drawer class="drawer" #drawer />
        } @else if (alt.profileOpen() && ws.mission()) {
          <fpv-altitude-drawer class="drawer" #drawer />
        }
        @if (replay.importOpen()) {
          <fpv-flight-import />
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
    .modes { flex: none; }
    .modes ::ng-deep .rf-segmented > button { white-space: nowrap; height: 32px; }
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
  protected readonly replay = inject(FlightReplay);
  protected readonly alt = inject(AltitudeView);
  protected readonly modes: SegmentOption<WorkspaceMode>[] = [
    { value: 'plan', label: 'Plan' },
    { value: 'replay', label: 'Replay' },
    { value: 'plan-fact', label: 'Plan vs fact' },
  ];

  protected setMode(mode: WorkspaceMode): void {
    void this.replay.setMode(mode);
  }
  protected readonly padding = { top: 70, bottom: 70, left: 80, right: 300 };
  /** How much of the map the bottom drawer covers, px: overlays on the map (the chase timeline) sit above it. */
  protected readonly cover = signal(0);
  private readonly drawer = viewChild('drawer', { read: ElementRef });

  constructor() {
    const ro = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver((e) => this.cover.set(Math.round(e[0]?.contentRect.height ?? 0)));
    effect((onCleanup) => {
      const el = this.drawer()?.nativeElement as HTMLElement | undefined;
      if (!el) {
        this.cover.set(0);
        return;
      }
      this.cover.set(el.offsetHeight);
      ro?.observe(el);
      onCleanup(() => ro?.unobserve(el));
    });
    inject(DestroyRef).onDestroy(() => ro?.disconnect());
  }
}
