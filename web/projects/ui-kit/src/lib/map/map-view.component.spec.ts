import { TestBed } from '@angular/core/testing';
import type { LayerSpecification, Map as MlMap, SourceSpecification } from 'maplibre-gl';
import { describe, expect, it } from 'vitest';
import { StyleLayerSet } from '../layers/layer-definition';
import { LayerRegistry } from '../layers/layer-registry';
import { SlotRegistry } from '../slots/slot-registry';
import { MapContext, provideMapView } from './map-context';
import { MapViewComponent } from './map-view.component';

/** A layer a module bundle contributes through slot `map.layers`. */
class ModuleLayer extends StyleLayerSet {
  readonly title = 'Module layer';
  readonly group = 'results' as const;
  constructor(readonly id: string) {
    super();
  }
  protected sources(): Record<string, SourceSpecification> {
    return {};
  }
  protected layers(): LayerSpecification[] {
    return [];
  }
}

/**
 * The view is created and checked, but never rendered by the application, so no MapLibre map
 * (which needs WebGL) is created; only the slot bridge and the teardown run.
 */
function createView() {
  TestBed.configureTestingModule({ providers: [provideMapView()] });
  const fixture = TestBed.createComponent(MapViewComponent);
  return {
    fixture,
    layers: TestBed.inject(LayerRegistry),
    slots: TestBed.inject(SlotRegistry),
    ctx: TestBed.inject(MapContext),
    check: () => fixture.componentRef.changeDetectorRef.detectChanges(),
  };
}

describe('MapViewComponent', () => {
  it('bridges map.layers slot contributions into the layer registry and drops removed ones', () => {
    const { fixture, layers, slots, check } = createView();
    const off = slots.register({ id: 'heat', slot: 'map.layers', layer: new ModuleLayer('module-heat') });
    check();
    expect(layers.get('module-heat')).toBeDefined();
    off();
    check();
    expect(layers.get('module-heat')).toBeUndefined();
    fixture.destroy();
  });

  it('releases the map context when destroyed', () => {
    const { fixture, ctx } = createView();
    ctx.map.set({} as MlMap);
    fixture.destroy();
    expect(ctx.map()).toBeNull();
  });
});
