import { TestBed } from '@angular/core/testing';
import { describe, expect, it } from 'vitest';
import { RfFormat } from '../format/rf-format';
import { BudgetTerm } from '../models/api.models';
import { BudgetChartComponent, waterfall } from './budget-chart.component';

const terms: BudgetTerm[] = [
  { id: 'tx', label: 'Tx power', valueDb: -27, source: 'datasheet' },
  { id: 'fspl', label: 'Free space', valueDb: 100, source: 'ITU-R P.525' },
  { id: 'free-space-extra', label: 'Free space (2nd)', valueDb: 3, source: 'model' },
  { id: 'cable', label: 'Cable', valueDb: 2, source: 'setup' },
];

describe('waterfall', () => {
  it('accumulates the level and tells gains, path loss and other losses apart', () => {
    const steps = waterfall(terms);
    expect(steps.map((s) => [s.from, s.to, s.kind])).toEqual([
      [0, 27, 'gain'],
      [27, -73, 'path'],
      [-73, -76, 'path'],
      [-76, -78, 'loss'],
    ]);
  });
});

describe('BudgetChartComponent', () => {
  class BudgetChartPage {
    readonly fixture = TestBed.createComponent(BudgetChartComponent);

    async render(inputs: { terms?: BudgetTerm[]; receivedDbm?: number; requiredDbm?: number }) {
      for (const [k, v] of Object.entries(inputs)) this.fixture.componentRef.setInput(k, v);
      await this.fixture.whenStable();
      return this;
    }

    get svg(): SVGElement | null {
      return (this.fixture.nativeElement as HTMLElement).querySelector('svg');
    }

    get barKinds(): string[] {
      return [...(this.fixture.nativeElement as HTMLElement).querySelectorAll('rect.bar')].map((r) =>
        r.getAttribute('class')!.replace('bar ', ''),
      );
    }

    get received(): Element | null {
      return (this.fixture.nativeElement as HTMLElement).querySelector('circle.received');
    }
  }

  it('draws nothing without terms', async () => {
    const page = await new BudgetChartPage().render({ terms: [] });
    expect(page.svg).toBeNull();
  });

  it('uses the last level as received level and names no requirement when none is given', async () => {
    const page = await new BudgetChartPage().render({ terms });
    expect(page.barKinds).toEqual(['gain', 'path', 'path', 'loss']);
    expect(page.svg!.getAttribute('aria-label')).toBe(`Link budget waterfall: received ${RfFormat.format(-78, 'dbm')}`);
    expect(page.received!.getAttribute('class')).toContain('ok');
  });

  it('marks a received level below the requirement as bad and names the requirement', async () => {
    const page = await new BudgetChartPage().render({ terms, receivedDbm: -95, requiredDbm: -90 });
    expect(page.svg!.getAttribute('aria-label')).toBe(
      `Link budget waterfall: received ${RfFormat.format(-95, 'dbm')}, required ${RfFormat.format(-90, 'dbm')}`,
    );
    expect(page.received!.getAttribute('class')).toContain('bad');
    expect((page.fixture.nativeElement as HTMLElement).querySelector('line.required')).not.toBeNull();
  });
});
