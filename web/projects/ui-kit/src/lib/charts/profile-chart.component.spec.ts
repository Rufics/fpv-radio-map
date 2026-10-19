import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Diffraction, Profile } from '../models/api.models';
import { ProfileChartComponent } from './profile-chart.component';

/** jsdom has no ResizeObserver; the chart only needs observe/disconnect. */
class NoopResizeObserver {
  observe(): void {}
  disconnect(): void {}
}

const hill: Profile = {
  points: [
    { distanceM: 0, groundMslM: 100 },
    { distanceM: 1000, groundMslM: 200 },
    { distanceM: 2000, groundMslM: 100 },
  ],
};

const open: Profile = {
  points: [
    { distanceM: 0, groundMslM: 100 },
    { distanceM: 1000, groundMslM: 90 },
    { distanceM: 2000, groundMslM: 100 },
  ],
};

class ProfileChartPage {
  readonly fixture = TestBed.createComponent(ProfileChartComponent);

  async render(inputs: Record<string, unknown>): Promise<this> {
    for (const [k, v] of Object.entries(inputs)) this.fixture.componentRef.setInput(k, v);
    await this.fixture.whenStable();
    return this;
  }

  private get host(): HTMLElement {
    return this.fixture.nativeElement as HTMLElement;
  }

  get aria(): string | null | undefined {
    return this.host.querySelector('svg')?.getAttribute('aria-label');
  }

  get obstacleText(): string | undefined {
    return this.host.querySelector('.obstacle-text')?.firstChild?.textContent?.trim();
  }

  get obstacleTitle(): string | undefined {
    return this.host.querySelector('.obstacle-text title')?.textContent ?? undefined;
  }

  get obstacleBox(): { x: number; w: number } | null {
    const box = this.host.querySelector('.obstacle-box');
    return box ? { x: Number(box.getAttribute('x')), w: Number(box.getAttribute('width')) } : null;
  }

  get intrusionCount(): number {
    return this.host.querySelectorAll('path.intrusion').length;
  }

  get emptyText(): string | undefined {
    return this.host.querySelector('.empty')?.textContent ?? undefined;
  }
}

describe('ProfileChartComponent', () => {
  beforeEach(() => vi.stubGlobal('ResizeObserver', NoopResizeObserver));
  afterEach(() => vi.unstubAllGlobals());

  it('says so when there is no usable profile', async () => {
    const page = await new ProfileChartPage().render({ profile: { points: [{ distanceM: 0, groundMslM: 1 }] } });
    expect(page.emptyText).toContain('No terrain profile');
  });

  it('shows a clear first Fresnel zone without an obstacle label', async () => {
    const page = await new ProfileChartPage().render({ profile: open, fromAglM: 20, toAglM: 20 });
    expect(page.aria).toContain('First Fresnel zone is clear.');
    expect(page.obstacleBox).toBeNull();
    expect(page.intrusionCount).toBe(0);
  });

  it('labels the worst intrusion with its height and the given loss', async () => {
    const page = await new ProfileChartPage().render({ profile: hill, fromAglM: 2, toAglM: 2, lossDb: 12, obstacleName: 'Ridge' });
    expect(page.intrusionCount).toBe(1);
    expect(page.aria).toContain('Terrain enters the first Fresnel zone');
    expect(page.obstacleText).toMatch(/^Ridge 200 m · .*12/);
    expect(page.obstacleTitle).toMatch(/^edge 200(\.\d+)? m MSL at 1 km, loss 12 dB$/);
  });

  it('places the label over the diffraction edge and keeps it inside the plot', async () => {
    const diffraction: Diffraction = { lineOfSight: false, edgeHeightM: 150, d1M: 1990 };
    const page = await new ProfileChartPage().render({ profile: open, diffraction });
    expect(page.obstacleText).toMatch(/^Obstacle 150 m$/);
    expect(page.obstacleTitle).not.toContain('loss');
    const box = page.obstacleBox!;
    // default size 1000 px wide, right plot edge at 1000 − 40
    expect(box.x + box.w).toBeLessThanOrEqual(960);
  });

  it('takes the edge from the dominant point and the loss from the diffraction result', async () => {
    const diffraction: Diffraction = {
      lineOfSight: false,
      lossDb: 7,
      dominant: { distanceM: 500, groundMslM: 95 },
    };
    const page = await new ProfileChartPage().render({ profile: open, diffraction });
    expect(page.obstacleText).toMatch(/^Obstacle 95 m · .*7/);
    expect(page.obstacleTitle).toBe('edge 95 m MSL at 0.5 km, loss 7 dB');
  });
});
