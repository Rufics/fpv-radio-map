import {
  Component,
  ElementRef,
  computed,
  effect,
  inject,
  input,
  untracked,
  viewChild,
} from '@angular/core';
import {
  CoverageGrid,
  CoverageLayer,
  CoverageProbe,
  HeatScale,
  LatLon,
  MapContext,
  RfFormat,
} from '@rfmap/ui-kit';
import { MapPointer, ViewBounds } from './map-pointer';

/** What the mini-map draws besides the coverage. */
export interface MiniMapScene {
  route: LatLon[];
  view: ViewBounds | null;
  cursor: LatLon | null;
}

/**
 * Draws the coverage mini-map on a canvas: the whole grid in full colour, the route, the part of
 * the world the main map shows and the point under the mouse. Positions go through the probe, so
 * they match the heatmap the main map would draw.
 */
export class MiniMapPainter {
  private readonly scale = new HeatScale(0, 255);
  private image: HTMLCanvasElement | null = null;
  private imageFor: CoverageGrid | null = null;

  /** Size in CSS pixels for a box at most `max` wide or tall, keeping the grid's Mercator aspect. */
  static size(probe: CoverageProbe, max: number): { width: number; height: number } {
    const [w, s, e, n] = probe.bounds;
    const dx = ((e - w) * Math.PI) / 180;
    const dy = CoverageProbe.mercatorY(n) - CoverageProbe.mercatorY(s);
    const aspect = dx > 0 && dy > 0 ? dx / dy : 1;
    return aspect >= 1
      ? { width: max, height: Math.round(max / aspect) }
      : { width: Math.round(max * aspect), height: max };
  }

  paint(
    canvas: HTMLCanvasElement,
    grid: CoverageGrid,
    probe: CoverageProbe,
    scene: MiniMapScene,
  ): void {
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const W = canvas.width;
    const H = canvas.height;
    const at = (p: LatLon) => {
      const f = probe.fraction(p);
      return [f.x * W, f.y * H] as const;
    };
    ctx.fillStyle = '#0E1319';
    ctx.fillRect(0, 0, W, H);
    this.drawCoverage(ctx, grid, W, H);
    this.drawRoute(ctx, scene.route, at, W);
    if (scene.view) this.drawView(ctx, scene.view, at, W);
    if (scene.cursor) this.drawCursor(ctx, at(scene.cursor), W);
  }

  private drawCoverage(
    ctx: CanvasRenderingContext2D,
    grid: CoverageGrid,
    W: number,
    H: number,
  ): void {
    if (this.imageFor !== grid) {
      this.image = CoverageLayer.paintCanvas(grid, this.scale);
      this.imageFor = grid;
    }
    if (!this.image) return;
    ctx.imageSmoothingEnabled = true;
    ctx.globalAlpha = 0.9;
    ctx.drawImage(this.image, 0, 0, W, H);
    ctx.globalAlpha = 1;
  }

  private drawRoute(
    ctx: CanvasRenderingContext2D,
    route: LatLon[],
    at: (p: LatLon) => readonly [number, number],
    W: number,
  ): void {
    if (route.length < 2) return;
    const px = W / 180;
    ctx.lineJoin = 'round';
    ctx.strokeStyle = 'rgba(11, 15, 20, 0.8)';
    ctx.lineWidth = 4 * px;
    this.path(ctx, route, at);
    ctx.strokeStyle = '#E7ECF2';
    ctx.lineWidth = 1.6 * px;
    this.path(ctx, route, at);
    const [x, y] = at(route[0]);
    ctx.fillStyle = '#35D0BA';
    ctx.beginPath();
    ctx.arc(x, y, 3.5 * px, 0, Math.PI * 2);
    ctx.fill();
  }

  private path(
    ctx: CanvasRenderingContext2D,
    route: LatLon[],
    at: (p: LatLon) => readonly [number, number],
  ): void {
    ctx.beginPath();
    route.forEach((p, i) => {
      const [x, y] = at(p);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  private drawView(
    ctx: CanvasRenderingContext2D,
    view: ViewBounds,
    at: (p: LatLon) => readonly [number, number],
    W: number,
  ): void {
    const [x0, y0] = at({ lat: view[3], lon: view[0] });
    const [x1, y1] = at({ lat: view[1], lon: view[2] });
    ctx.strokeStyle = 'rgba(231, 236, 242, 0.55)';
    ctx.lineWidth = W / 180;
    ctx.setLineDash([4 * (W / 180), 3 * (W / 180)]);
    ctx.strokeRect(x0, y0, x1 - x0, y1 - y0);
    ctx.setLineDash([]);
  }

  private drawCursor(
    ctx: CanvasRenderingContext2D,
    [x, y]: readonly [number, number],
    W: number,
  ): void {
    const px = W / 180;
    ctx.lineWidth = 2 * px;
    ctx.strokeStyle = '#0B0F14';
    ctx.beginPath();
    ctx.arc(x, y, 6 * px, 0, Math.PI * 2);
    ctx.stroke();
    ctx.strokeStyle = '#E7ECF2';
    ctx.lineWidth = 1.4 * px;
    ctx.stroke();
  }
}

/**
 * Coverage mini-map: the heatmap moves off the main map into a small synced overview in the
 * corner, where it can be drawn at full strength without covering the route. It follows the main
 * map's view and the mouse, reads out the margin under the mouse, and a click on it recentres the
 * main map there.
 */
@Component({
  selector: 'fpv-coverage-mini-map',
  template: `
    @if (probe(); as p) {
      <div class="mini" [style.width.px]="box().width">
        <canvas
          #canvas
          [style.width.px]="box().width"
          [style.height.px]="box().height"
          role="img"
          [attr.aria-label]="'Coverage mini-map. ' + readout()"
          (click)="recentre($event)"
        ></canvas>
        <div class="cap">
          <span>Coverage</span><b class="rf-mono">{{ readout() }}</b>
        </div>
      </div>
    }
  `,
  styles: `
    :host {
      position: absolute;
      left: 14px;
      bottom: 40px;
      z-index: 3;
    }
    .mini {
      border-radius: 10px;
      overflow: hidden;
      border: 1px solid var(--rf-border-strong);
      background: rgba(16, 22, 29, 0.95);
      box-shadow: 0 6px 20px rgba(0, 0, 0, 0.45);
    }
    canvas {
      display: block;
      cursor: pointer;
    }
    .cap {
      display: flex;
      justify-content: space-between;
      gap: 8px;
      padding: 5px 8px;
      font-size: 11px;
      color: var(--rf-text-2);
    }
    .cap b {
      color: var(--rf-text);
      font-weight: 500;
    }
  `,
})
export class CoverageMiniMapComponent {
  readonly grid = input<CoverageGrid | null>(null);
  /** Pilot first, then the waypoints in flight order (and the pilot again for a return). */
  readonly route = input<LatLon[]>([]);
  private readonly pointer = inject(MapPointer);
  private readonly ctx = inject(MapContext);
  private readonly painter = new MiniMapPainter();
  private readonly canvas = viewChild<ElementRef<HTMLCanvasElement>>('canvas');

  protected readonly probe = computed(() => CoverageProbe.of(this.grid()));
  protected readonly box = computed(() => {
    const p = this.probe();
    return p ? MiniMapPainter.size(p, 180) : { width: 180, height: 180 };
  });
  protected readonly readout = computed(() => {
    const c = this.pointer.cursor();
    const p = this.probe();
    if (!c || !p) return 'hover the map';
    const m = p.marginAt(c);
    if (m === null) return 'no link';
    return RfFormat.format(m, 'db+');
  });

  constructor() {
    effect(() => {
      const el = this.canvas()?.nativeElement;
      const grid = this.grid();
      const probe = this.probe();
      const scene: MiniMapScene = {
        route: this.route(),
        view: this.pointer.view(),
        cursor: this.pointer.cursor(),
      };
      const { width, height } = this.box();
      if (!el || !grid || !probe) return;
      untracked(() => {
        const dpr = globalThis.devicePixelRatio || 1;
        el.width = Math.round(width * dpr);
        el.height = Math.round(height * dpr);
        this.painter.paint(el, grid, probe, scene);
      });
    });
  }

  protected recentre(e: MouseEvent): void {
    const p = this.probe();
    const map = this.ctx.map();
    const el = e.target as HTMLElement;
    if (!p || !map) return;
    const r = el.getBoundingClientRect();
    const at = p.pointAt((e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height);
    map.easeTo({ center: [at.lon, at.lat], duration: 400 });
  }
}
