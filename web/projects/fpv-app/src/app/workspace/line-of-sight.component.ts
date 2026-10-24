import { Component, DestroyRef, computed, effect, inject, input, untracked } from '@angular/core';
import { Marker } from 'maplibre-gl';
import {
  CoverageGrid,
  CoverageProbe,
  LatLon,
  LayerRegistry,
  LineOfSightLayer,
  LinkKind,
  LinkResult,
  MapContext,
  Mission,
  RfFormat,
  RfmapApiClient,
  SightTone,
  distanceM,
} from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { MapPointer } from './map-pointer';

/** What the line of sight says about the point under the mouse. */
export interface SightReading {
  marginDb: number | null;
  distanceM: number;
  /** From the server's path calculation: clear line of sight or the terrain that blocks it. */
  exact?: { lineOfSight: boolean; edgeHeightM?: number; obstruction?: LatLon | null };
  /** The exact check could not run (no terrain data there, a failed request). */
  failed?: boolean;
}

/** Turns a reading into the tag text and tone. */
export class SightReadout {
  static tone(r: SightReading): SightTone {
    return LineOfSightLayer.tone(r.marginDb);
  }

  static text(r: SightReading): string {
    const margin = r.marginDb === null ? 'no link' : RfFormat.format(r.marginDb, 'db+');
    const parts = [margin, RfFormat.format(r.distanceM, 'km1')];
    if (r.exact)
      parts.push(
        r.exact.lineOfSight ? 'clear line of sight' : SightReadout.blocked(r.exact.edgeHeightM),
      );
    else if (r.failed) parts.push('no terrain data');
    else parts.push('checking…');
    return parts.join(' · ');
  }

  private static blocked(edgeHeightM: number | undefined): string {
    return edgeHeightM === undefined
      ? 'blocked by terrain'
      : `blocked by terrain ${RfFormat.format(edgeHeightM, 'm')}`;
  }

  /** The exact part of a reading from the server's link result. */
  static fromResult(res: LinkResult): Pick<SightReading, 'marginDb' | 'exact'> {
    const d = res.diffraction;
    const dom = d?.dominant;
    const los = d?.lineOfSight ?? true;
    const obstruction =
      !los && dom?.lat !== undefined && dom.lon !== undefined
        ? { lat: dom.lat, lon: dom.lon }
        : null;
    return {
      marginDb: res.marginDb ?? null,
      exact: { lineOfSight: los, edgeHeightM: d?.edgeHeightM ?? dom?.groundMslM, obstruction },
    };
  }
}

/** Pause of the mouse before the exact path calculation runs. */
const SETTLE_MS = 450;

/**
 * Line of sight on hover (analysis only): a line from the pilot to the point under the mouse,
 * coloured by the coverage margin there, with a tag that reads out the margin and the distance.
 * When the mouse rests, the server calculates that one path and the tag says whether the line is
 * clear or which ridge blocks it, and the ridge is marked on the line.
 */
@Component({
  selector: 'fpv-line-of-sight',
  template: '',
})
export class LineOfSightComponent {
  readonly active = input(false);
  readonly grid = input<CoverageGrid | null>(null);
  readonly link = input<LinkKind>('control');
  private readonly pointer = inject(MapPointer);
  private readonly ctx = inject(MapContext);
  private readonly ws = inject(FpvWorkspace);
  private readonly api = inject(RfmapApiClient);
  private readonly layer = new LineOfSightLayer();
  private readonly probe = computed(() => CoverageProbe.of(this.grid()));
  private tag?: Marker;
  private tagLeft = false;
  private timer?: ReturnType<typeof setTimeout>;
  /** Bumped by every new point, so a late answer for an old one is dropped. */
  private seq = 0;

  constructor() {
    const off = inject(LayerRegistry).register(this.layer);
    effect(() => {
      const mission = this.ws.mission();
      const cursor = this.active() ? this.pointer.cursor() : null;
      const probe = this.probe();
      const link = this.link();
      untracked(() => this.follow(mission, cursor, probe, link));
    });
    inject(DestroyRef).onDestroy(() => {
      this.clear();
      off();
    });
  }

  private follow(
    mission: Mission | null,
    cursor: LatLon | null,
    probe: CoverageProbe | null,
    link: LinkKind,
  ): void {
    clearTimeout(this.timer);
    const seq = ++this.seq;
    if (!mission || !cursor) {
      this.clear();
      return;
    }
    const reading: SightReading = {
      marginDb: probe?.marginAt(cursor) ?? null,
      distanceM: distanceM(mission.pilot, cursor),
    };
    this.show(mission.pilot, cursor, reading);
    const setup = mission.links[link];
    if (!setup) return;
    this.timer = setTimeout(() => void this.check(seq, mission, cursor, link, reading), SETTLE_MS);
  }

  private async check(
    seq: number,
    mission: Mission,
    to: LatLon,
    link: LinkKind,
    reading: SightReading,
  ): Promise<void> {
    const aglM = mission.coverage?.droneAglM ?? 120;
    try {
      const res = await this.api.calcPath(this.ws.store.missionId(), {
        from: mission.pilot,
        to: { lat: to.lat, lon: to.lon, aglM },
        kind: link,
        link: mission.links[link]!,
      });
      if (seq === this.seq)
        this.show(mission.pilot, to, { ...reading, ...SightReadout.fromResult(res) });
    } catch {
      if (seq === this.seq) this.show(mission.pilot, to, { ...reading, failed: true });
    }
  }

  private show(from: LatLon, to: LatLon, r: SightReading): void {
    const tone = SightReadout.tone(r);
    this.layer.setSight({ from, to, tone, obstruction: r.exact?.obstruction ?? null });
    const map = this.ctx.map();
    if (!map) return;
    // the tag goes on the side of the point with more room, so it never runs off the map
    const left = map.project([to.lon, to.lat]).x > map.getContainer().clientWidth * 0.6;
    if (!this.tag || this.tagLeft !== left) {
      this.tag?.remove();
      const el = document.createElement('div');
      el.style.pointerEvents = 'none';
      el.appendChild(document.createElement('div'));
      this.tag = new Marker({
        element: el,
        anchor: left ? 'bottom-right' : 'bottom-left',
        offset: left ? [-12, -12] : [12, -12],
      });
      this.tagLeft = left;
    }
    // the marker element keeps MapLibre's own classes; the tag is its child
    const tag = this.tag.getElement().firstElementChild as HTMLElement;
    tag.className = `rf-map-tag ${SIGHT_TAG_CLASS[tone]}`;
    tag.textContent = SightReadout.text(r);
    // only a settled reading is announced, not every step of the mouse
    if (r.exact || r.failed) tag.setAttribute('role', 'status');
    else tag.removeAttribute('role');
    this.tag.setLngLat([to.lon, to.lat]).addTo(map);
  }

  private clear(): void {
    clearTimeout(this.timer);
    this.layer.setSight(null);
    this.tag?.remove();
  }
}

const SIGHT_TAG_CLASS: Readonly<Record<SightTone, string>> = {
  ok: '',
  warn: 'rf-map-tag--warn',
  blocked: 'rf-map-tag--blocked',
  unknown: 'rf-map-tag--muted',
};
