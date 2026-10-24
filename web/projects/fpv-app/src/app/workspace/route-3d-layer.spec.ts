import { describe, expect, it, vi } from 'vitest';
import { CustomRenderMethodInput, Map as MlMap, MercatorCoordinate } from 'maplibre-gl';
import { corridorOf } from '../tests/altitude-fixtures';
import { fakeGl } from '../tests/fake-gl';
import { FpvAltitudeData } from '../core/fpv-altitude';
import {
  LINE_STRIDE,
  POINT_STRIDE,
  ROUTE_3D_COLOURS,
  Route3dCursor,
  Route3dLayer,
  Route3dScene,
  TRI_STRIDE,
  layerMatrix,
} from './route-3d-layer';
import { terrainSource } from './route-3d.component';

const data: FpvAltitudeData = {
  pilot: { lat: 48.5, lon: 35.2, groundMslM: 500, antennaMslM: 501.5 },
  corridor: corridorOf(),
  waypoints: [
    { index: 1, label: 'B', s: 3000, groundMslM: 450, mslM: 600, aglM: 150, aboveLaunchM: 100 },
  ],
  fresnel: { link: 'control', frequencyHz: 915e6 },
};

describe('Route3dScene', () => {
  const opts = { exaggeration: 1, corridor: false };

  it('draws the curtain, the shadow, the glowing line and the posts', () => {
    const scene = new Route3dScene(data, { ...opts, exaggeration: 2, corridor: true });
    // The curtain: two triangles per gap.
    expect(scene.triangles.length / TRI_STRIDE).toBe(3 * 2 * 3);
    // Shadow, glow and core of 3 segments each; the dashed floor and ceiling on every other gap;
    // a post at the pilot and at B. 6 vertices a segment.
    expect(scene.lines.length / LINE_STRIDE).toBe((3 * 3 + 2 * 2 + 2) * 6);
    // A ground pin and a ball at each post, and a label on top.
    expect(scene.points.length / POINT_STRIDE).toBe(4);
    expect(scene.labels.map((l) => l.text)).toEqual(['Pilot', 'B · 100 m above launch · 150 m AGL']);
    // Heights are exaggerated; positions are relative to the pilot.
    expect(scene.triangles.slice(0, 3)).toEqual([0, 0, 1000]);
    // The curtain fades toward the ground.
    expect(scene.triangles[6]).toBeLessThan(scene.triangles[TRI_STRIDE * 2 + 6]);
    const firstCore = (3 + 2 + 2 + 3) * 6 * LINE_STRIDE;
    expect(scene.lines.slice(firstCore + 9, firstCore + 13)).toEqual([...ROUTE_3D_COLOURS.takeoff]);
    // The glow under it is the same colour, faint and wide.
    const firstGlow = (3 + 2 + 2) * 6 * LINE_STRIDE;
    expect(scene.lines[firstGlow + 8]).toBeGreaterThan(scene.lines[firstCore + 8]);
    expect(scene.lines[firstGlow + 12]).toBeLessThan(0.5);
  });

  it('leaves the corridor lines out when they are off', () => {
    const scene = new Route3dScene(data, opts);
    expect(scene.triangles.length / TRI_STRIDE).toBe(3 * 2 * 3);
    expect(scene.lines.length / LINE_STRIDE).toBe((3 * 3 + 2) * 6);
  });

  it('drops a dashed line from the cursor to the ground with a read-out', () => {
    const cursor = new Route3dCursor(data, { exaggeration: 1, fresnel: false, cursorS: 1500 });
    expect(cursor.lines.length / LINE_STRIDE).toBe(12 * 6);
    expect(cursor.points.length / POINT_STRIDE).toBe(1);
    expect(cursor.triangles).toEqual([]);
    expect(cursor.labels.length).toBe(1);
    const tip = cursor.labels[0];
    expect(tip.text).toMatch(/^1\.5 km · \d+ m AGL · /);
    expect(['ok', 'warn']).toContain(tip.tone);
  });

  it('draws the Fresnel zone to the cursor only when asked', () => {
    const plain = new Route3dCursor(data, { exaggeration: 1, fresnel: false, cursorS: 1500 });
    const fresnel = new Route3dCursor(data, { exaggeration: 1, fresnel: true, cursorS: 1500 });
    expect((fresnel.triangles.length - plain.triangles.length) / TRI_STRIDE).toBe(24 * 16 * 6);
    // Its widest ring is the first-zone radius half way, around the line from the antenna to the drone.
    const k = MercatorCoordinate.fromLngLat([35.2, 48.5], 0).meterInMercatorCoordinateUnits();
    const extra = fresnel.triangles.slice(plain.triangles.length);
    let widest = 0;
    for (let i = 0; i < extra.length; i += TRI_STRIDE)
      widest = Math.max(widest, Math.abs(extra[i] / k));
    expect(widest).toBeCloseTo(12, 0);
  });

  it('refuses data without a corridor', () => {
    expect(
      () =>
        new Route3dScene({}, { exaggeration: 1, corridor: true }),
    ).toThrow();
  });
});

describe('Route3dLayer', () => {
  const scene = () =>
    new Route3dScene(data, { exaggeration: 2, corridor: true });
  const map = () =>
    ({
      getZoom: () => 12,
      getPixelRatio: () => 2,
      triggerRepaint: vi.fn(),
    }) as unknown as MlMap;
  const args = { modelViewProjectionMatrix: new Float64Array(16) } as unknown as CustomRenderMethodInput;

  it('uploads the scene once and draws triangles, lines and points', () => {
    const gl = fakeGl();
    const m = map();
    const layer = new Route3dLayer();
    layer.onAdd(m, gl);
    layer.render(gl, args);
    expect(gl.drawArrays).not.toHaveBeenCalled();

    const s = scene();
    layer.setScene(s);
    expect(m.triggerRepaint).toHaveBeenCalled();
    layer.render(gl, args);
    layer.render(gl, args);
    expect(gl.bufferData).toHaveBeenCalledTimes(3);
    expect(gl.drawArrays).toHaveBeenCalledWith(gl.TRIANGLES, 0, s.triangles.length / TRI_STRIDE);
    expect(gl.drawArrays).toHaveBeenCalledWith(gl.TRIANGLES, 0, s.lines.length / LINE_STRIDE);
    expect(gl.drawArrays).toHaveBeenCalledWith(gl.POINTS, 0, s.points.length / POINT_STRIDE);
    // The line viewport is in CSS pixels.
    expect(gl.uniform2f).toHaveBeenCalledWith(expect.anything(), 400, 300);

    layer.onRemove(m, gl);
    expect(gl.deleteProgram).toHaveBeenCalledTimes(3);
    // three buffers for the route and three for the cursor
    expect(gl.deleteBuffer).toHaveBeenCalledTimes(6);
    vi.mocked(gl.drawArrays).mockClear();
    layer.render(gl, args);
    expect(gl.drawArrays).not.toHaveBeenCalled();
  });

  it('uploads only the cursor when the cursor moves', () => {
    const gl = fakeGl();
    const layer = new Route3dLayer();
    layer.onAdd(map(), gl);
    layer.setScene(scene());
    layer.render(gl, args);
    vi.mocked(gl.bufferData).mockClear();
    const cursor = new Route3dCursor(data, { exaggeration: 2, fresnel: false, cursorS: 1500 });
    layer.setCursor(cursor);
    layer.render(gl, args);
    expect(gl.bufferData).toHaveBeenCalledTimes(3);
    expect(vi.mocked(gl.bufferData).mock.calls.map((c) => (c[1] as Float32Array).length)).toEqual([
      0,
      cursor.lines.length,
      cursor.points.length,
    ]);
    // a cursor without a route draws nothing
    layer.setScene(null);
    vi.mocked(gl.drawArrays).mockClear();
    layer.render(gl, args);
    expect(gl.drawArrays).not.toHaveBeenCalled();
  });

  it('keeps one label element per scene label and moves it to its point', () => {
    const gl = fakeGl();
    const layer = new Route3dLayer();
    const host = document.createElement('div');
    layer.onAdd(map(), gl);
    layer.setLabelHost(host);
    layer.setScene(scene());
    expect([...host.children].map((e) => e.textContent)).toEqual([
      'Pilot',
      'B · 100 m above launch · 150 m AGL',
    ]);
    expect(host.children[0].className).toBe('fpv-3d-label is-point');
    // the cursor's read-out comes after the route's labels
    layer.setCursor(new Route3dCursor(data, { exaggeration: 2, fresnel: false, cursorS: 1500 }));
    expect(host.children.length).toBe(3);
    expect(host.children[2].className).toMatch(/is-(ok|warn)/);
    layer.setCursor(null);
    // A view that maps every point to the centre puts the labels in the middle of the 400×300 viewport.
    const centre = new Float64Array(16);
    centre[15] = 1;
    layer.render(gl, { modelViewProjectionMatrix: centre } as unknown as CustomRenderMethodInput);
    const pilot = host.children[0] as HTMLElement;
    expect(pilot.hidden).toBe(false);
    expect(pilot.style.transform).toBe('translate(200.0px, 150.0px)');
    // Behind the camera, labels hide.
    layer.render(gl, args);
    expect(pilot.hidden).toBe(true);
    layer.setScene(null);
    expect(host.children.length).toBe(0);
  });

  it('skips attributes the shader does not use', () => {
    const gl = fakeGl({ getAttribLocation: () => -1 });
    const layer = new Route3dLayer();
    layer.onAdd(map(), gl);
    layer.setScene(scene());
    layer.render(gl, args);
    expect(gl.enableVertexAttribArray).not.toHaveBeenCalled();
  });

  it('reports a shader that does not compile or link', () => {
    expect(() =>
      new Route3dLayer().onAdd(map(), fakeGl({ getShaderParameter: () => false })),
    ).toThrow('compile error');
    expect(() =>
      new Route3dLayer().onAdd(map(), fakeGl({ getProgramParameter: () => false })),
    ).toThrow('link error');
    expect(() => new Route3dLayer().onAdd(map(), fakeGl({ createShader: () => null }))).toThrow(
      'WebGL shader could not be created',
    );
  });
});

describe('layerMatrix', () => {
  it('folds the origin and the world size into the view-projection', () => {
    const id = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    const m = layerMatrix(id, { x: 0.25, y: 0.5 }, 512);
    // A scene point (dx, dy, z) lands at ((0.25 + dx)·512, (0.5 + dy)·512, z).
    const apply = (p: number[]) =>
      [0, 1, 2].map((r) => m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r]);
    expect(apply([0.001, -0.002, 300])).toEqual([
      expect.closeTo(128.512, 3),
      expect.closeTo(254.976, 3),
      300,
    ]);
  });
});

describe('terrainSource', () => {
  it('reads pmtiles and XYZ dem sources', () => {
    expect(terrainSource({ pmtiles: '/tiles/terrain.pmtiles', encoding: 'terrarium' })).toEqual({
      type: 'raster-dem',
      url: 'pmtiles:///tiles/terrain.pmtiles',
      encoding: 'terrarium',
      tileSize: 256,
    });
    expect(terrainSource({ tiles: ['mockdem://{z}/{x}/{y}'], maxzoom: 13 })).toEqual({
      type: 'raster-dem',
      tiles: ['mockdem://{z}/{x}/{y}'],
      encoding: 'terrarium',
      tileSize: 256,
      maxzoom: 13,
    });
  });
});
