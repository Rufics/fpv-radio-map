// Example runtime module UI bundle for FPV Radio Map (module "fpv.example").
// Plain ES module, no framework: the app imports it from ModuleInfo.ui.bundle and calls register(api).
// It may only use the slots its manifest declares in ui.slots (here: side.panels, verdict.reasons).

export const kit = '1';

export function register(api) {
  api.slots.register({
    id: 'flight-heights',
    slot: 'side.panels',
    order: 50,
    title: 'Flight heights',
    mount(host) {
      host.classList.add('rf-card');
      host.style.cssText = 'display:flex;flex-direction:column;gap:6px;padding:12px;border-radius:12px;border:1px solid var(--rf-border);background:var(--rf-surface-1);font-size:13px';
      const head = document.createElement('div');
      head.innerHTML = '<b>Flight heights</b> <span class="rf-badge rf-badge--beta">beta · fpv.example</span>';
      const body = document.createElement('div');
      body.style.color = 'var(--rf-text-3)';
      host.append(head, body);

      const off = api.state.subscribe(({ mission }) => {
        body.replaceChildren();
        if (!mission || mission.waypoints.length === 0) {
          body.textContent = 'Place a route to see the heights above ground.';
          return;
        }
        const heights = mission.waypoints.map((p) => p.aglM);
        const max = Math.max(...heights);
        const min = Math.min(...heights);
        const line = document.createElement('div');
        line.textContent = `Lowest ${api.format.format(min, 'm')} · highest ${api.format.format(max, 'm')} above ground`;
        line.title = `${api.format.full(min, 'm')} – ${api.format.full(max, 'm')}`;
        body.append(line);
        if (max > 120) {
          const warn = document.createElement('div');
          warn.style.color = 'var(--rf-warn-text)';
          warn.textContent = '! Above 120 m AGL: check the local airspace rules.';
          body.append(warn);
        }
      });
      return off;
    },
  });

  api.slots.register({
    id: 'verdict-note',
    slot: 'verdict.reasons',
    order: 90,
    mount(host) {
      host.style.cssText = 'font-size:12px;color:var(--rf-text-2)';
      host.textContent = `Note from ${api.moduleId} ${api.moduleVersion}: example module, no effect on the verdict.`;
    },
  });
}
