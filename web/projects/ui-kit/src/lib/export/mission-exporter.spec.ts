import { Mission } from '../models/mission.models';
import { MissionExporter, xmlEscape } from './mission-exporter';

const mission: Mission = {
  schema: 'mission/v1',
  name: 'North ridge <recon> & co',
  time: { mode: 'now' },
  pilot: { lat: 48.53, lon: 35.2, aglM: 2, label: 'A · pilot' },
  waypoints: [
    { lat: 48.557338, lon: 35.171784, aglM: 120 },
    { lat: 48.571728, lon: 35.210852, aglM: 120 },
    { lat: 48.5572762, lon: 35.2586279, aglM: 120 },
  ],
  returnToPilot: true,
  links: {},
  evaluate: 'both',
};

function parse(xml: string): Document {
  const doc = new DOMParser().parseFromString(xml, 'application/xml');
  expect(doc.getElementsByTagName('parsererror').length).toBe(0);
  return doc;
}

describe('MissionExporter', () => {
  const exporter = new MissionExporter();

  it('escapes XML', () => {
    expect(xmlEscape(`a<b>&"'`)).toBe('a&lt;b&gt;&amp;&quot;&apos;');
  });

  it('writes valid KML with every point, full precision and AGL heights', () => {
    const file = exporter.toKml(mission, {
      // legs in route order, index 0-based as the server sends them
      legs: [
        { index: 0, status: 'ok' },
        { index: 1, status: 'ok' },
        { index: 2, status: 'blocked', worst: { video: { marginDb: -3.74265 }, control: { marginDb: 6.01041 } } },
      ],
    });
    expect(file.filename).toBe('north-ridge-recon-co.kml');
    expect(file.mime).toContain('kml');
    const doc = parse(file.content);
    const placemarks = doc.getElementsByTagName('Placemark');
    expect(placemarks.length).toBe(5); // pilot + 3 waypoints + route
    expect(doc.getElementsByTagName('name')[0].textContent).toBe('North ridge <recon> & co');
    const names = Array.from(placemarks).map((p) => p.getElementsByTagName('name')[0].textContent);
    expect(names).toEqual(['A · pilot', 'WP1', 'WP2', 'B · target', 'Route']);
    const line = doc.getElementsByTagName('LineString')[0].getElementsByTagName('coordinates')[0].textContent ?? '';
    const coords = line.trim().split(' ');
    expect(coords.length).toBe(5); // returns to pilot
    expect(coords[3]).toBe('35.2586279,48.5572762,120');
    expect(coords[4]).toBe('35.2,48.53,2');
    expect(file.content).toContain('<Data name="videoMarginDb"><value>-3.74265</value></Data>');
    // the margins belong to the third point (B), not to the first
    const bMark = Array.from(placemarks).find((p) => p.getElementsByTagName('name')[0].textContent === 'B · target');
    expect(bMark?.textContent).toContain('-3.74265');
    expect(file.content).toContain('relativeToGround');
  });

  it('writes valid GPX 1.1 with waypoints and a route', () => {
    const file = exporter.toGpx({ ...mission, returnToPilot: false });
    expect(file.filename).toBe('north-ridge-recon-co.gpx');
    const doc = parse(file.content);
    expect(doc.documentElement.getAttribute('version')).toBe('1.1');
    expect(doc.getElementsByTagName('wpt').length).toBe(4);
    const rtepts = doc.getElementsByTagName('rtept');
    expect(rtepts.length).toBe(4);
    expect(rtepts[3].getAttribute('lat')).toBe('48.5572762');
    expect(rtepts[3].getAttribute('lon')).toBe('35.2586279');
    // no MSL elevation is invented; AGL goes in the extension
    expect(doc.getElementsByTagName('ele').length).toBe(0);
    expect(file.content).toContain('<rfmap:aglM>120</rfmap:aglM>');
  });

  it('adds the return leg to the GPX route when the mission returns to the pilot', () => {
    const doc = parse(exporter.toGpx(mission).content);
    const rtepts = doc.getElementsByTagName('rtept');
    expect(rtepts.length).toBe(5);
    expect(rtepts[4].getElementsByTagName('name')[0].textContent).toBe('A · return');
  });

  it('exports launch-referenced heights without placing them above the ground', () => {
    const launch: Mission = { ...mission, altitudeRef: 'launch', waypoints: mission.waypoints.map((w) => ({ lat: w.lat, lon: w.lon, altM: 150 })) };
    const kml = exporter.toKml(launch);
    const doc = parse(kml.content);
    const marks = Array.from(doc.getElementsByTagName('Placemark'));
    const wp1 = marks.find((p) => p.getElementsByTagName('name')[0].textContent === 'WP1');
    expect(wp1?.getElementsByTagName('altitudeMode')[0].textContent).toBe('clampToGround');
    expect(wp1?.textContent).toContain('150');
    expect(kml.content).toContain('<Data name="altM">');
    const pilot = marks.find((p) => p.getElementsByTagName('name')[0].textContent === 'A · pilot');
    expect(pilot?.getElementsByTagName('altitudeMode')[0].textContent).toBe('relativeToGround');

    const gpx = exporter.toGpx(launch).content;
    expect(gpx).toContain('<rfmap:altM>150</rfmap:altM>');
    expect(gpx).toContain('<rfmap:aglM>2</rfmap:aglM>');
    expect(gpx).toContain('above the launch point');
  });
});
