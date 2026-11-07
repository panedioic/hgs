import { UIAbility } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { window } from '@kit.ArkUI';
import { HgsService, HgsServiceOptions, HgsServiceWindow, HgsSession, HgsStartError } from 'hgs';
import probe from 'libhgs_x11_probe.so';

function delay(ms: number): Promise<void> {
  return new Promise<void>((resolve: () => void): void => {
    setTimeout(resolve, ms);
  });
}

export default class EntryAbility extends UIAbility {
  private report: string[] = [];

  onWindowStageCreate(stage: window.WindowStage): void {
    stage.loadContent('pages/Index');
    this.run();
  }

  private record(message: string): void {
    this.report.push(message);
    const file = fs.openSync(this.context.filesDir + '/hgs-p2-report.log',
      fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
    try {
      fs.writeSync(file.fd, this.report.join('\n') + '\n');
    } finally {
      fs.closeSync(file);
    }
  }

  private async client(session: HgsSession, name: string): Promise<HgsServiceWindow> {
    const environment = session.clientEnvironment('x11');
    const title = 'HGS X11 ' + name;
    const log = this.context.filesDir + '/hgs-p2-' + name + '.log';
    const child = probe.launch([environment.set['DISPLAY'], environment.set['XAUTHORITY'], log, title].join('\n'));
    if (child < 0) throw new Error('probe launch failed ' + child);
    this.record('probe ' + name + ' started');
    let target: HgsServiceWindow | undefined;
    for (let i = 0; i < 80 && target === undefined; i++) {
      target = session.windows().find((w: HgsServiceWindow): boolean => w.title === title);
      if (target === undefined) await delay(50);
    }
    if (!target) throw new Error('X11 window did not appear: ' + name);
    this.record('mapped ' + JSON.stringify(target));
    const id = target.id;
    if (!session.sendInput(id, { kind: 'pointer', action: 1, x: 40, y: 40, button: 272, fromMouse: true }) ||
        !session.sendInput(id, { kind: 'pointer', action: 2, x: 40, y: 40, button: 272, fromMouse: true })) {
      throw new Error('pointer routing failed');
    }
    await delay(150);
    if (!session.sendInput(id, { kind: 'key', evdevCode: 30, pressed: true }) ||
        !session.sendInput(id, { kind: 'key', evdevCode: 30, pressed: false })) {
      throw new Error('key routing failed');
    }
    for (let i = 0; i < 100; i++) {
      if (!session.windows().some((w: HgsServiceWindow): boolean => w.id === id)) {
        const result = fs.readTextSync(log);
        this.record(result);
        if (!result.includes('AUTH_REJECTED') || !result.includes('FRAME OK') || !result.includes('\nPASS\n')) {
          throw new Error('X11 probe failed: ' + name);
        }
        return target;
      }
      await delay(100);
    }
    throw new Error('client did not close: ' + name);
  }

  private async run(): Promise<void> {
    const service = HgsService.create();
    try {
      this.record('P2 test starting');
      const base = this.context.filesDir;
      const data = this.context.resourceManager.getRawFileContentSync('hgs-x11-res.zip');
      const zip = base + '/hgs-x11-res.zip';
      const file = fs.openSync(zip, fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
      try {
        fs.writeSync(file.fd, data.buffer);
      } finally {
        fs.closeSync(file);
      }
      await zlib.decompressFile(zip, base, {});
      const options: HgsServiceOptions = {
        protocols: 'x11', runtimeDir: base + '/hgs-p2-runtime', composition: 'per-window',
        x11: { xkbRoot: base + '/hgs-x11/xkb', keymapPath: base + '/hgs-x11/keymap.xkm' }
      };
      const broken: HgsServiceOptions = {
        protocols: 'both', runtimeDir: options.runtimeDir, composition: 'per-window',
        wayland: { socketName: 'rollback-wayland' },
        x11: { xkbRoot: base + '/missing-xkb', keymapPath: base + '/hgs-x11/keymap.xkm' }
      };
      let rejected = false;
      try {
        await service.start(broken);
      } catch (failure) {
        const error = failure as HgsStartError;
        rejected = error.code === 'invalid-config' && error.stage === 'x11-resources';
      }
      if (!rejected || service.inspect().endpoints.length !== 0 ||
          fs.accessSync(options.runtimeDir + '/rollback-wayland')) {
        throw new Error('failed both startup did not roll back the Wayland listener');
      }
      this.record('both missing-resource rollback passed');
      const session = await service.start(options);
      this.record('x11 ready ' + JSON.stringify(service.inspect()));
      const authority = session.clientEnvironment('x11').set['XAUTHORITY'];
      const display = session.clientEnvironment('x11').set['DISPLAY'];
      const first = await this.client(session, 'first');
      const second = await this.client(session, 'second');
      if (first.epoch !== second.epoch || first.id === second.id || first.clientId === second.clientId ||
          service.inspect().state !== 'ready') {
        throw new Error('reconnection identity or lifetime mismatch');
      }
      await service.stop();
      if (fs.accessSync(authority)) throw new Error('stopped X11 authority file is still present');
      this.record('x11 stopped ' + JSON.stringify(service.inspect()));

      options.protocols = 'both';
      options.wayland = { socketName: 'p2-wayland' };
      const both = await service.start(options);
      this.record('both ready ' + JSON.stringify(service.inspect()));
      if (both.endpoints.length !== 2 || both.epoch === session.epoch) {
        throw new Error('both endpoint count or epoch mismatch');
      }
      if (both.clientEnvironment('x11').set['DISPLAY'] !== display) {
        throw new Error('old X11 listener was not released');
      }
      this.record('X11 listener rebound ' + display);
      await this.client(both, 'both');
      await service.stop();
      this.record('P2 TEST PASS');
    } catch (e) {
      this.record('P2 TEST FAIL ' + String(e));
      await service.stop();
    }
  }
}
