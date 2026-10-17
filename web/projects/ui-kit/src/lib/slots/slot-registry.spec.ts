import { Component } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { SlotRegistrationError, SlotRegistry } from './slot-registry';
import { SlotHostComponent } from './slot-host.component';

@Component({ selector: 'test-panel', template: 'panel from app' })
class TestPanel {}

describe('SlotRegistry', () => {
  let registry: SlotRegistry;
  beforeEach(() => {
    registry = TestBed.inject(SlotRegistry);
  });

  it('registers into named slots, sorted by order', () => {
    registry.register({ id: 'b', slot: 'side.panels', order: 20, mount: () => undefined });
    registry.register({ id: 'a', slot: 'side.panels', order: 10, mount: () => undefined });
    registry.register({ id: 'x', slot: 'header.actions', mount: () => undefined });
    expect(registry.slot('side.panels')().map((c) => c.id)).toEqual(['a', 'b']);
    expect(registry.slot('header.actions')().map((c) => c.id)).toEqual(['x']);
  });

  it('returns an unregister function', () => {
    const off = registry.register({ id: 'a', slot: 'bottom.drawer', mount: () => undefined });
    expect(registry.slot('bottom.drawer')().length).toBe(1);
    off();
    expect(registry.slot('bottom.drawer')().length).toBe(0);
  });

  it('rejects unknown slots, duplicates and ambiguous contributions', () => {
    expect(() => registry.register({ id: 'a', slot: 'nope' as never, mount: () => undefined })).toThrow(SlotRegistrationError);
    registry.register({ id: 'a', slot: 'side.panels', mount: () => undefined });
    expect(() => registry.register({ id: 'a', slot: 'side.panels', mount: () => undefined })).toThrow(/already/);
    expect(() => registry.register({ id: 'c', slot: 'side.panels' })).toThrow(/exactly one/);
    expect(() =>
      registry.register({ id: 'd', slot: 'side.panels', layer: { id: 'l' } as never }),
    ).toThrow(/map.layers/);
  });

  it('removes all contributions of one module', () => {
    registry.register({ id: 'a', slot: 'side.panels', source: 'mod.x', mount: () => undefined });
    registry.register({ id: 'b', slot: 'verdict.reasons', source: 'mod.x', mount: () => undefined });
    registry.register({ id: 'c', slot: 'side.panels', source: 'app', mount: () => undefined });
    expect(registry.unregisterSource('mod.x')).toBe(2);
    expect(registry.all().map((c) => c.id)).toEqual(['c']);
  });

  it('slot host renders components and mount functions and runs cleanup', async () => {
    let cleaned = false;
    registry.register({ id: 'cmp', slot: 'side.panels', order: 1, component: TestPanel });
    registry.register({
      id: 'dom',
      slot: 'side.panels',
      order: 2,
      source: 'mod.example',
      mount: (host) => {
        host.textContent = 'panel from module';
        return () => (cleaned = true);
      },
    });
    const fixture = TestBed.createComponent(SlotHostComponent);
    fixture.componentRef.setInput('name', 'side.panels');
    fixture.detectChanges();
    await fixture.whenStable();
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('panel from app');
    expect(text).toContain('panel from module');
    fixture.destroy();
    expect(cleaned).toBe(true);
  });

  it('isolates a failing mount function', async () => {
    registry.register({
      id: 'bad',
      slot: 'side.panels',
      source: 'mod.bad',
      mount: () => {
        throw new Error('boom');
      },
    });
    const fixture = TestBed.createComponent(SlotHostComponent);
    fixture.componentRef.setInput('name', 'side.panels');
    fixture.detectChanges();
    await fixture.whenStable();
    expect((fixture.nativeElement as HTMLElement).textContent).toContain('mod.bad');
    expect((fixture.nativeElement as HTMLElement).textContent).toContain('boom');
  });
});
