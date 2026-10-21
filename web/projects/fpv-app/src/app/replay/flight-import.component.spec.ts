import { signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { FlightLogPreview, FlightView, MissionStore, RfmapApiClient } from '@rfmap/ui-kit';
import { FlightReplay } from '../core/flight-replay.service';
import { MockFlights } from '../mock/mock-flights';
import { FlightImportComponent } from './flight-import.component';

interface ImportVm {
  files: () => { file: File; state: string; error?: string }[];
  choice: () => { file: number; flight: number } | null;
  chosenView: () => FlightView | null;
  over: () => boolean;
  busy: () => boolean;
  error: () => string | null;
  save: { set(v: boolean): void };
  add(files: File[]): void;
  dragOver(e: unknown): void;
  dropped(e: unknown): void;
  chosen(e: unknown): void;
  isChosen(file: number, flight: number): boolean;
  summaryOf(f: unknown): string;
  size(bytes: number): string;
  time(v: FlightView): string;
  line(v: FlightView): string;
  replayChosen(): Promise<void>;
}

/** Opening log files: what each file holds, which flight is chosen, and keeping them. */
describe('FlightImportComponent', () => {
  let replay: FlightReplay;
  let vm: ImportVm;
  const missionId = signal<string | null>('m1');
  const canEdit = signal(true);
  const preview = (name: string, flights: FlightView[]): FlightLogPreview => ({
    decoder: 'fpv.blackbox',
    source: { decoder: 'fpv.blackbox', decoderVersion: '1.0.0', format: 'betaflight-blackbox', fileName: name, bytes: 10 },
    flights,
  });
  const api = {
    decodeFlightLog: vi.fn(async (_m: string | null, file: File, name: string) => {
      if (name === 'broken.bbl') throw new Error('the decoder could not read it');
      return preview(name, name.startsWith('btfl') ? [MockFlights.headerOnly(name)] : [MockFlights.flight(name, null)]);
    }),
    importFlightLog: vi.fn(async (_m: string, _f: File, name: string) => ({
      decoder: 'fpv.blackbox',
      fileName: name,
      flights: [MockFlights.row({ ...MockFlights.flight(name, null), id: `id-${name}` })],
    })),
    listFlightLogs: vi.fn(async () => []),
    getFlightLog: vi.fn(async (_m: string, id: string) => ({ ...MockFlights.flight('a.bbl', null), id })),
  };
  const file = (name: string, size = 1000) => ({ name, size }) as File;
  const settled = () => vi.waitFor(() => expect(vm.files().every((f) => f.state !== 'reading')).toBe(true));

  beforeEach(() => {
    missionId.set('m1');
    canEdit.set(true);
    TestBed.configureTestingModule({
      providers: [
        { provide: RfmapApiClient, useValue: api },
        { provide: MissionStore, useValue: { missionId, canEdit } },
      ],
    });
    TestBed.overrideComponent(FlightImportComponent, { set: { template: '', imports: [] } });
    replay = TestBed.inject(FlightReplay);
    replay.importOpen.set(true);
    vm = TestBed.createComponent(FlightImportComponent).componentInstance as unknown as ImportVm;
  });

  afterEach(() => {
    replay.pause();
    vi.clearAllMocks();
  });

  it('reads each file and chooses the first flight that can be replayed', async () => {
    vm.add([file('btfl_016.bbl'), file('huge.bfl', 65 * 1024 * 1024), file('broken.bbl'), file('LOG00007.BFL', 3 * 1024 * 1024)]);
    await settled();
    const [headerOnly, huge, broken, log] = vm.files();
    expect(headerOnly.state).toBe('done');
    expect(vm.summaryOf(headerOnly)).toMatch(/^ · 0 flights · read in \d+\.\d s$/);
    expect(huge.error).toBe('huge.bfl is 65.0 MB; files up to 64 MB are read.');
    expect(broken).toMatchObject({ state: 'error', error: 'the decoder could not read it' });
    expect(vm.summaryOf(broken)).toBe('');
    expect(vm.summaryOf(log)).toMatch(/^ · 1 flight/);
    expect(vm.choice()).toEqual({ file: 3, flight: 0 });
    expect(vm.isChosen(3, 0)).toBe(true);
    expect(vm.isChosen(0, 0)).toBe(false);
    expect(vm.chosenView()?.status).toBe('ok');
    expect(vm.size(300)).toBe('1 KB');
    expect(vm.time(vm.chosenView()!)).toBe('15:15 UTC');
    expect(vm.line(vm.chosenView()!)).toContain(' · ');
  });

  it('takes files dropped or chosen in the file picker', async () => {
    const prevent = vi.fn();
    vm.dragOver({ preventDefault: prevent });
    expect(vm.over()).toBe(true);
    vm.dropped({ preventDefault: prevent, dataTransfer: { files: [file('a.bbl')] } });
    expect(vm.over()).toBe(false);
    vm.dropped({ preventDefault: prevent });
    const input = { files: [file('b.bbl')], value: 'b.bbl' };
    vm.chosen({ target: input });
    expect(input.value).toBe('');
    vm.chosen({ target: { files: null, value: '' } });
    await settled();
    expect(vm.files().map((f) => f.file.name)).toEqual(['a.bbl', 'b.bbl']);
    expect(prevent).toHaveBeenCalledTimes(3);
  });

  it('only shows the flight when it is not kept', async () => {
    await vm.replayChosen();
    expect(replay.view()).toBeNull();
    vm.add([file('a.bbl')]);
    await settled();
    vm.save.set(false);
    await vm.replayChosen();
    expect(api.importFlightLog).not.toHaveBeenCalled();
    expect(replay.view()?.fileName).toBe('a.bbl');
    expect(replay.importOpen()).toBe(false);
  });

  it('keeps every file with flights, then opens the chosen one from the logbook', async () => {
    vm.add([file('a.bbl'), file('btfl_016.bbl'), file('b.bbl')]);
    await settled();
    await vm.replayChosen();
    expect(api.importFlightLog.mock.calls.map((c) => c[2])).toEqual(['a.bbl', 'b.bbl']);
    expect(api.listFlightLogs).toHaveBeenCalled();
    expect(api.getFlightLog).toHaveBeenCalledWith('m1', 'id-a.bbl');
    expect(replay.importOpen()).toBe(false);
    expect(vm.busy()).toBe(false);
  });

  it('shows the flight when the import gives no stored row, and says why an import failed', async () => {
    vm.add([file('a.bbl')]);
    await settled();
    missionId.set(null);
    await vm.replayChosen();
    expect(replay.view()?.fileName).toBe('a.bbl');

    missionId.set('m1');
    replay.importOpen.set(true);
    api.importFlightLog.mockRejectedValueOnce(new Error('a mission keeps up to 200 flights'));
    await vm.replayChosen();
    expect(vm.error()).toBe('a mission keeps up to 200 flights');
    expect(replay.importOpen()).toBe(true);
  });
});
