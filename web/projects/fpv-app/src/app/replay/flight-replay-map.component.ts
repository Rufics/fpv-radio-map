import { Component, DestroyRef, computed, effect, inject, input, untracked } from '@angular/core';
import { Map as MlMap, Marker } from 'maplibre-gl';
import { FlightStoryEvent, FlightTrackLayer, LayerRegistry, LatLon, MapContext, TrackScale } from '@rfmap/ui-kit';
import { FlightReplay } from '../core/flight-replay.service';

/** Positions kept when fitting the map to a long track: enough for its outline. */
const FIT_POINTS = 2000;

/**
 * The replayed flight on the map: the flown track coloured by the chosen value, the craft at the
 * replay time, and a numbered marker per story event (the story list uses the same numbers).
 * Outside replay it draws nothing.
 */
@Component({
  selector: 'fpv-flight-replay-map',
  template: `
    @if (shown() && legend().length) {
      <div class="legend" role="note" [attr.aria-label]="'Track coloured by ' + title()">
        <b>Track colour · {{ title() }}</b>
        <div class="keys">
          @for (k of legend(); track k.label) {
            <span><i [style.background]="k.color"></i>{{ k.label }}</span>
          }
        </div>
      </div>
    }
  `,
  styles: `
    :host { position: absolute; inset: 0; pointer-events: none; z-index: 2; }
    .legend { position: absolute; left: 14px; top: 16px; padding: 8px 10px; border-radius: var(--rf-radius-lg); background: rgba(16, 22, 29, 0.95); border: 1px solid var(--rf-border); font-size: 11px; color: var(--rf-text-2); display: flex; flex-direction: column; gap: 6px; }
    .legend b { color: var(--rf-text); font-weight: 600; font-size: 12px; }
    .keys { display: flex; gap: 10px; flex-wrap: wrap; }
    .keys span { display: inline-flex; align-items: center; gap: 4px; }
    .keys i { width: 14px; height: 5px; border-radius: 2px; }
    :host-context(.phone) .legend { display: none; }
  `,
})
export class FlightReplayMapComponent {
  private readonly replay = inject(FlightReplay);
  private readonly layers = inject(LayerRegistry);
  private readonly ctx = inject(MapContext);
  readonly fitPadding = input<{ top: number; bottom: number; left: number; right: number }>({ top: 70, bottom: 70, left: 80, right: 80 });

  protected readonly shown = computed(() => this.replay.mode() !== 'plan' && !!this.replay.timeline()?.hasTrack);
  protected readonly title = computed(() => TrackScale.title(this.replay.colourBy()));
  protected readonly legend = computed(() => TrackScale.legend(this.replay.colourBy()));

  private readonly layer = new FlightTrackLayer();
  private markers: Pick<Marker, 'remove'>[] = [];

  constructor() {
    const off = this.layers.register(this.layer);
    effect(() => {
      const tl = this.shown() ? this.replay.timeline() : null;
      const metric = this.replay.colourBy();
      untracked(() => this.layer.setTrack(tl ? tl.segments(metric) : []));
    });
    effect(() => {
      const at = this.shown() ? (this.replay.reading()?.position ?? null) : null;
      untracked(() => this.layer.setCursor(at));
    });
    effect(() => {
      const map = this.ctx.map();
      const story = this.shown() ? (this.replay.view()?.story ?? []) : [];
      const selected = this.replay.selected();
      untracked(() => this.drawMarkers(map, story, selected));
    });
    // A new flight on screen: show all of it.
    effect(() => {
      const tl = this.shown() ? this.replay.timeline() : null;
      const map = this.ctx.map();
      if (!tl || !map) return;
      untracked(() => this.ctx.fitTo(FlightReplayMapComponent.outline(tl.positions()), this.fitPadding()));
    });
    inject(DestroyRef).onDestroy(() => {
      off();
      this.drawMarkers(null, [], null);
    });
  }

  /** Every n-th position, so a long track does not overflow the bounds maths. */
  static outline(points: LatLon[]): LatLon[] {
    const step = Math.max(1, Math.ceil(points.length / FIT_POINTS));
    return points.filter((_, i) => i % step === 0 || i === points.length - 1);
  }

  private drawMarkers(map: MlMap | null, story: FlightStoryEvent[], selected: number | null): void {
    this.markers.forEach((m) => m.remove());
    this.markers = [];
    if (!map) return;
    story.forEach((e, i) => {
      if (e.lat === undefined || e.lon === undefined) return;
      const el = document.createElement('button');
      el.type = 'button';
      el.className = `rf-flight-mark is-${e.severity}${i === selected ? ' is-selected' : ''}`;
      el.textContent = String(i + 1);
      el.setAttribute('aria-label', `${i + 1}. ${e.title}`);
      el.addEventListener('click', (ev) => {
        ev.stopPropagation();
        this.replay.selectEvent(i);
      });
      this.markers.push(this.placeMarker(map, el, [e.lon, e.lat]));
    });
  }

  private placeMarker(map: MlMap, el: HTMLElement, at: [number, number]): Pick<Marker, 'remove'> {
    return new Marker({ element: el, anchor: 'center' }).setLngLat(at).addTo(map);
  }
}
