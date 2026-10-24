import { Component } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { beforeEach, describe, expect, it } from 'vitest';
import { CoverageLayer } from '../coverage/coverage-layer';
import { LayerPanelComponent } from './layer-panel.component';
import { LayerRegistry } from './layer-registry';
import { createStandardLayers } from './standard-layers';

/** The panel with the options a host projects into its Coverage section and at the end. */
@Component({
  imports: [LayerPanelComponent],
  template: `
    <rf-layer-panel>
      <label rfCoverageOptions class="host-coverage">Mini-map</label>
      <div class="host-view">View</div>
    </rf-layer-panel>
  `,
})
class HostComponent {}

describe('LayerPanelComponent', () => {
  let registry: LayerRegistry;

  beforeEach(() => {
    TestBed.configureTestingModule({ providers: [LayerRegistry] });
    registry = TestBed.inject(LayerRegistry);
    const tiles = { tiles: ['https://tiles.example/{z}/{x}/{y}.png'] };
    const config = {
      basemap: tiles,
      satellite: tiles,
      dem: tiles,
      contours: tiles,
      weather: tiles,
    };
    for (const def of createStandardLayers(config, '2026-10-24T12:00:00Z')) registry.register(def);
    registry.register(new CoverageLayer());
  });

  function render() {
    const fixture = TestBed.createComponent(HostComponent);
    fixture.detectChanges();
    const el = fixture.nativeElement as HTMLElement;
    const sections = () =>
      [...el.querySelectorAll('rf-layer-section')].map((s) => ({
        title: s.querySelector('.title')?.textContent?.trim(),
        badge: s.querySelector('.badge')?.textContent?.trim() ?? null,
        open: s.querySelector('details')?.open ?? false,
      }));
    return { fixture, el, sections };
  }

  it('groups the layers into sections, with the coverage options in Coverage', () => {
    const { el, sections } = render();
    expect(sections()).toEqual([
      { title: 'Map', badge: null, open: true },
      { title: 'Coverage', badge: '1 on', open: true },
      // weather starts closed while none of its layers is on
      { title: 'Weather', badge: null, open: false },
    ]);
    const coverage = el.querySelectorAll('rf-layer-section')[1];
    expect(coverage.querySelector('.host-coverage')).not.toBeNull();
    expect(coverage.querySelector('rf-heat-legend')).not.toBeNull();
    expect(el.querySelector('.host-view')?.closest('rf-layer-section')).toBeNull();
  });

  it('switches the base map and toggles a layer from its checkbox', () => {
    const { fixture, el, sections } = render();
    const satellite = [...el.querySelectorAll<HTMLButtonElement>('[role="radio"]')].find((b) =>
      b.textContent?.includes('Satellite'),
    )!;
    satellite.click();
    fixture.detectChanges();
    expect(satellite.getAttribute('aria-checked')).toBe('true');

    const wind = [...el.querySelectorAll('label.toggle')].find((l) =>
      l.textContent?.includes('Wind'),
    )!;
    const box = wind.querySelector('input')!;
    box.checked = true;
    box.dispatchEvent(new Event('change'));
    fixture.detectChanges();
    expect(sections()[2].badge).toBe('1 on');
    expect(registry.isVisible('weather-wind')).toBe(true);
  });

  it('keeps a section as the user left it when its layers change', () => {
    const { fixture, el, sections } = render();
    const weather = el.querySelectorAll('rf-layer-section')[2].querySelector('details')!;
    weather.open = true;
    weather.dispatchEvent(new Event('toggle'));
    registry.setVisible('weather-wind', true);
    fixture.detectChanges();
    registry.setVisible('weather-wind', false);
    fixture.detectChanges();
    expect(sections()[2].open).toBe(true);
  });
});
