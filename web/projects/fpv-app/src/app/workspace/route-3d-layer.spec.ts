import { describe, expect, it, vi } from 'vitest';
import { CustomRenderMethodInput, Map as MlMap, MercatorCoordinate } from 'maplibre-gl';
import { corridorOf } from '../tests/altitude-fixtures';
import { fakeGl } from '../tests/fake-gl';
import { FpvAltitudeData } from '../core/fpv-altitude';
import {
  LINE_STRIDE,
  ROUTE_3D_COLOURS,
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
  it('draws the curtain, corridor, flight line and posts', () => {
    const scene = new Route3dScene(data, {
      exaggeration: 2,
      corridor: true,
      fresnel: false,
      cursorS: null,
    });
    // Two walls (curtain and corridor) of two triangles per gap.
    expect(scene.triangles.length / TRI_STRIDE).toBe(3 * 2 * 2 * 3);
    // Floor, ceiling and plan of 3 segments each, plus posts at the pilot and B, 6 vertices each.
    expect(scene.lines.length / LINE_STRIDE).toBe((3 * 3 + 2) * 6);
    // Heights are exaggerated; positions are relative to the pilot.
    expect(scene.triangles.slice(0, 3)).toEqual([0, 0, 1000]);
    const firstPlan = 3 * 2 * 6 * LINE_STRIDE;
    expect(scene.lines.slice(firstPlan + 9, firstPlan + 13)).toEqual([...ROUTE_3D_COLOURS.takeoff]);
  });

  it('leaves the corridor out when it is off', () => {
    const scene = new Route3dScene(data, {
      exaggeration: 1,
      corridor: false,
      fresnel: false,
      cursorS: null,
    });
    expect(scene.triangles.length / TRI_STRIDE).toBe(3 * 2 * 3);
    expect(scene.lines.length / LINE_STRIDE).toBe((3 + 2) * 6);
  });

  it('draws the Fresnel zone to the cursor only when asked', () => {
    const plain = new Route3dScene(data, {
      exaggeration: 1,
      corridor: false,
      fresnel: false,
      cursorS: 1500,
    });
    const fresnel = new Route3dScene(data, {
      exaggeration: 1,
      corridor: false,
      fresnel: true,
      cursorS: 1500,
    });
    expect(
      plain.lines.length -
        new Route3dScene(data, { exaggeration: 1, corridor: false, fresnel: false, cursorS: null })
          .lines.length,
    ).toBe(6 * LINE_STRIDE);
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
        new Route3dScene({}, { exaggeration: 1, corridor: true, fresnel: false, cursorS: null }),
    ).toThrow();
  });
});

describe('Route3dLayer', () => {
  const scene = () =>
    new Route3dScene(data, { exaggeration: 2, corridor: true, fresnel: false, cursorS: null });
  const map = () =>
    ({
      getZoom: () => 12,
      getPixelRatio: () => 2,
      triggerRepaint: vi.fn(),
    }) as unknown as MlMap;
  const args = { modelViewProjectionMatrix: new Float64Array(16) } as unknown as CustomRenderMethodInput;

  it('uploads the scene once and draws triangles and lines', () => {
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
    expect(gl.bufferData).toHaveBeenCalledTimes(2);
    expect(gl.drawArrays).toHaveBeenCalledWith(gl.TRIANGLES, 0, s.triangles.length / TRI_STRIDE);
    expect(gl.drawArrays).toHaveBeenCalledWith(gl.TRIANGLES, 0, s.lines.length / LINE_STRIDE);
    // The line viewport is in CSS pixels.
    expect(gl.uniform2f).toHaveBeenCalledWith(expect.anything(), 400, 300);

    layer.onRemove(m, gl);
    expect(gl.deleteProgram).toHaveBeenCalledTimes(2);
    expect(gl.deleteBuffer).toHaveBeenCalledTimes(2);
    vi.mocked(gl.drawArrays).mockClear();
    layer.render(gl, args);
    expect(gl.drawArrays).not.toHaveBeenCalled();
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
