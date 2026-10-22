import { describe, expect, it, vi } from 'vitest';
import type { Map as MlMap } from 'maplibre-gl';
import { FlightTrackLayer } from './flight-track-layer';

/** A map with GeoJSON sources whose data can be replaced, as the layer uses it. */
function fakeMap() {
  const sources = new Map<string, { setData: ReturnType<typeof vi.fn> }>();
  return {
    sources,
    getSource: (id: string) => sources.get(id),
    addSource: (id: string) => sources.set(id, { setData: vi.fn(async () => undefined) }),
    getLayer: () => undefined,
    addLayer: vi.fn(),
  };
}

describe('FlightTrackLayer', () => {
  it('draws the track segments in their colours and the cursor once on the map', () => {
    const layer = new FlightTrackLayer();
    layer.setTrack([{ points: [{ lat: 50, lon: 16 }], color: '#fff' }]);
    const map = fakeMap();
    layer.attach(map as unknown as MlMap, undefined);
    expect([...map.sources.keys()]).toEqual(['rf-flight-track', 'rf-flight-cursor']);
    expect(map.addLayer).toHaveBeenCalledTimes(4);

    layer.setTrack([
      { points: [{ lat: 50, lon: 16 }, { lat: 50.1, lon: 16.1 }], color: '#35D0BA' },
      { points: [{ lat: 50.1, lon: 16.1 }], color: '#FF6B5B' },
    ]);
    const track = map.sources.get('rf-flight-track')!.setData.mock.calls[0][0];
    expect(track.features.map((f: { properties: { color: string } }) => f.properties.color)).toEqual(['#35D0BA', '#FF6B5B']);
    expect(track.features[0].geometry.coordinates).toEqual([
      [16, 50],
      [16.1, 50.1],
    ]);

    layer.setCursor({ lat: 50.05, lon: 16.05 });
    layer.setCursor(null);
    const cursor = map.sources.get('rf-flight-cursor')!.setData.mock.calls.map((c) => c[0].features.length);
    expect(cursor).toEqual([1, 0]);
  });

  it('logs a source that could not take the new data', async () => {
    const layer = new FlightTrackLayer();
    const map = fakeMap();
    layer.attach(map as unknown as MlMap, undefined);
    map.sources.get('rf-flight-cursor')!.setData.mockRejectedValueOnce(new Error('gone'));
    const error = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    layer.setCursor({ lat: 1, lon: 2 });
    await Promise.resolve();
    await Promise.resolve();
    expect(error).toHaveBeenCalledWith('Flight track could not be updated', expect.any(Error));
    error.mockRestore();
  });
});
