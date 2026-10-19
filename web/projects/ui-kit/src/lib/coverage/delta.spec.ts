import type { FeatureCollection } from 'geojson';
import type { Map as MlMap } from 'maplibre-gl';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { CoverageGrid } from '../models/api.models';
import { CoverageDeltaLayer, DeltaScale, RouteDeltaLayer, sampleGrid, subtractGrids } from './delta';

// 2 × 2 cells of 0.1° from the north-west corner (35.0, 48.2)
const grid = (margins: (number | null)[], origin = { lat: 48.2, lon: 35.0 }, cols = 2, rows = 2): CoverageGrid => ({
  cols,
  rows,
  origin,
  cellLatDeg: 0.1,
  cellLonDeg: 0.1,
  bbox: [origin.lon, origin.lat - rows * 0.1, origin.lon + cols * 0.1, origin.lat],
  marginDb: margins,
});

describe('coverage difference B − A (exact cells only)', () => {
  it('subtracts identical grids cell by cell', () => {
    const d = subtractGrids(grid([10, 5, null, -2]), grid([13, 4, 7, -2]))!;
    expect(d.alignment).toBe('same');
    expect(d.grid.marginDb).toEqual([3, -1, null, 0]);
    expect(d.notComparableCells).toBe(0);
  });

  it('matches cells of a grid offset by whole cells, the rest is not comparable', () => {
    // B starts one column further east: B[c] coincides with A[c + 1]
    const b = grid([20, 21, 22, 23], { lat: 48.2, lon: 35.1 });
    const d = subtractGrids(grid([10, 5, 0, 1]), b)!;
    expect(d.alignment).toBe('shifted');
    expect(d.comparable).toEqual([false, true, false, true]);
    expect(d.grid.marginDb).toEqual([null, 20 - 5, null, 22 - 1]);
    expect(d.notComparableCells).toBe(2);
  });

  it('never resamples: a grid with other cells is not comparable anywhere', () => {
    const b: CoverageGrid = { ...grid([20, 20, 20, 20], { lat: 48.25, lon: 35.0 }) };
    const d = subtractGrids(grid([10, 5, 0, 1]), b)!;
    expect(d.alignment).toBe('none');
    expect(d.comparable.every((c) => !c)).toBe(true);
    expect(d.grid.marginDb!.every((v) => v === null)).toBe(true);
    const other = subtractGrids(grid([1, 1, 1, 1]), { ...grid([2, 2, 2, 2]), cellLonDeg: 0.11 })!;
    expect(other.alignment).toBe('none');
  });

  it('samples the cell containing a point, null outside', () => {
    const g = grid([1, 2, 3, 4]);
    expect(sampleGrid(g, 35.15, 48.15)).toBe(2);
    expect(sampleGrid(g, 40, 48)).toBeNull();
  });

  it('marks direction with text as well as colour', () => {
    const s = new DeltaScale(1);
    expect(s.mark(3)).toBe('▲');
    expect(s.mark(-3)).toBe('▼');
    expect(s.mark(0.4)).toBe('=');
    expect(s.color(0.5)).toBeNull();
    expect(s.color(5)?.slice(0, 3)).toEqual([...DeltaScale.BETTER]);
  });

  it('colours route segments grey inside the dead band, teal when better, coral when worse', () => {
    const s = new DeltaScale(1);
    expect(s.css(0.5)).toBe(`rgb(${DeltaScale.SAME.join(',')})`);
    expect(s.css(2)).toBe(`rgb(${DeltaScale.BETTER.join(',')})`);
    expect(s.css(-2)).toBe(`rgb(${DeltaScale.WORSE.join(',')})`);
  });
});

/** Image and GeoJSON sources of a fake map; GeoJSON setData rejects when `failing` is set. */
class FakeSource {
  data?: FeatureCollection;
  image?: { url: string };
  failing = false;
  updateImage(image: { url: string }): this {
    this.image = image;
    return this;
  }
  setData(fc: FeatureCollection): Promise<void> {
    this.data = fc;
    return this.failing ? Promise.reject(new Error('worker gone')) : Promise.resolve();
  }
}

class FakeMap {
  readonly sources = new Map<string, FakeSource>();
  readonly layers: string[] = [];
  readonly visibility = new Map<string, string>();
  getSource(id: string) {
    return this.sources.get(id);
  }
  addSource(id: string) {
    this.sources.set(id, new FakeSource());
  }
  getLayer(id: string) {
    return this.layers.includes(id) ? { id } : undefined;
  }
  addLayer(layer: { id: string }) {
    this.layers.push(layer.id);
  }
  setLayoutProperty(id: string, _name: string, value: string) {
    this.visibility.set(id, value);
  }
  asMap(): MlMap {
    return this as unknown as MlMap;
  }
}

/** jsdom has no 2D canvas: a context that keeps the last image put on it. */
class FakeCanvasContext {
  painted?: ImageData;
  createImageData(w: number, h: number) {
    return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) } as ImageData;
  }
  putImageData(img: ImageData) {
    this.painted = img;
  }
  /** RGBA of pixel x/y of the painted image. */
  pixel(x: number, y: number): number[] {
    const img = this.painted!;
    const o = (y * img.width + x) * 4;
    return [...img.data.slice(o, o + 4)];
  }
}

describe('CoverageDeltaLayer', () => {
  afterEach(() => vi.restoreAllMocks());

  function setup() {
    const canvas = new FakeCanvasContext();
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(canvas as never);
    vi.spyOn(HTMLCanvasElement.prototype, 'toDataURL').mockReturnValue('data:image/png;base64,delta');
    const map = new FakeMap();
    const layer = new CoverageDeltaLayer();
    layer.attach(map.asMap(), undefined);
    return { canvas, map, layer };
  }

  it('paints better cells teal and hatches cells that are not comparable', () => {
    const { canvas, map, layer } = setup();
    // B one column east: A cells 0 and 2 have no B cell
    const d = subtractGrids(grid([10, 5, 0, 1]), grid([20, 5.5, 22, 23], { lat: 48.2, lon: 35.1 }))!;
    layer.setDelta(d);
    expect(map.sources.get('rf-coverage-delta')!.image!.url).toBe('data:image/png;base64,delta');
    expect(map.visibility.get('coverage-delta')).toBe('visible');
    expect(canvas.painted!.width).toBe(8); // 2 cells × 4 px
    const hatch = [...DeltaScale.SAME, 150];
    expect(canvas.pixel(0, 0)).toEqual(hatch); // cell 0, on the diagonal
    expect(canvas.pixel(1, 0)).toEqual([0, 0, 0, 0]); // cell 0, between diagonals
    expect(canvas.pixel(4, 0).slice(0, 3)).toEqual([...DeltaScale.BETTER]); // cell 1: 20 − 5
    expect(canvas.pixel(4, 4).slice(0, 3)).toEqual([...DeltaScale.BETTER]); // cell 3: 23 − 1
  });

  it('leaves dead-band cells transparent', () => {
    const { canvas, layer } = setup();
    layer.setDelta(subtractGrids(grid([10, 10, 10, 10]), grid([10.5, 10, 9.5, 10]))!);
    expect(canvas.painted!.data.every((v) => v === 0)).toBe(true);
  });

  it('hides the layer when the difference is cleared', () => {
    const { map, layer } = setup();
    layer.setDelta(subtractGrids(grid([1, 1, 1, 1]), grid([5, 5, 5, 5]))!);
    layer.setDelta(null);
    expect(map.visibility.get('coverage-delta')).toBe('none');
  });
});

describe('RouteDeltaLayer', () => {
  afterEach(() => vi.restoreAllMocks());

  it('colours each route segment by the mean difference of its two samples', () => {
    const map = new FakeMap();
    const layer = new RouteDeltaLayer();
    layer.attach(map.asMap(), undefined);
    layer.setPoints([
      { lat: 48.5, lon: 35.0, deltaDb: 4 },
      { lat: 48.6, lon: 35.1, deltaDb: 2 },
      { lat: 48.7, lon: 35.2, deltaDb: -8 },
    ]);
    const fc = map.sources.get('rf-route-delta')!.data!;
    expect(fc.features.map((f) => f.properties?.['deltaDb'])).toEqual([3, -3]);
    expect(fc.features.map((f) => f.properties?.['color'])).toEqual([
      `rgb(${DeltaScale.BETTER.join(',')})`,
      `rgb(${DeltaScale.WORSE.join(',')})`,
    ]);
  });

  it('logs a failed source update', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    const map = new FakeMap();
    const layer = new RouteDeltaLayer();
    layer.attach(map.asMap(), undefined);
    map.sources.get('rf-route-delta')!.failing = true;
    layer.setPoints(null);
    await vi.waitFor(() => expect(error).toHaveBeenCalledWith('Route difference could not be updated', expect.any(Error)));
  });
});
