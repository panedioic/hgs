import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { fileIo as fs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { HgsService, HgsServiceEvent, HgsServiceOptions, HgsSession, HgsWindowHostService } from 'hgs';
import probe from 'libhgs_x11_probe.so';

// 独立宿主的 Want 决定监听模式。GTK 的交互结果写入单独的客户端日志。
export default class EntryAbility extends UIAbility {
  private protocols: 'wayland' | 'x11' | 'both' = 'both';
  private listener: number = 0;

  onCreate(want: Want, _param: AbilityConstant.LaunchParam): void {
    const requested = want.parameters?.protocols;
    if (requested === 'wayland') this.protocols = 'wayland';
    else if (requested === 'x11') this.protocols = 'x11';
  }

  async onWindowStageCreate(stage: window.WindowStage): Promise<void> {
    await stage.loadContent('pages/Index');
    const base = this.context.filesDir;
    const report = base + '/hgs-matrix.log';
    const file = fs.openSync(report, fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
    fs.closeSync(file);
    const service = HgsService.create();
    this.listener = service.onEvent((event: HgsServiceEvent): void => {
      this.append(report, JSON.stringify(event));
    });
    try {
      await this.unpack('hgs-x11-res.zip', base);
      if (this.protocols !== 'x11') await this.unpack('hgs-gtk-fonts.zip', base);
      HgsWindowHostService.init(this.context);
      const options: HgsServiceOptions = {
        protocols: this.protocols,
        runtimeDir: base + '/hgs-matrix',
        wayland: { socketName: 'hgs-matrix-wayland' },
        x11: { xkbRoot: base + '/hgs-x11/xkb', keymapPath: base + '/hgs-x11/keymap.xkm' }
      };
      const session = await HgsWindowHostService.startService(stage, options);
      const expected = this.protocols === 'both' ? 2 : 1;
      if (session.endpoints.length !== expected) throw new Error('Unexpected endpoint count');
      this.append(report, JSON.stringify(session.endpoints));
      if (this.protocols !== 'x11') this.launchGtk(session, base);
      if (this.protocols !== 'wayland') this.launchXlib(session, base);
    } catch (error) {
      this.append(report, String(error));
      this.unsubscribe();
      await HgsWindowHostService.stopService();
    }
  }

  async onWindowStageDestroy(): Promise<void> {
    this.unsubscribe();
    await HgsWindowHostService.stopService();
  }

  private unsubscribe(): void {
    if (this.listener === 0) return;
    HgsService.create().offEvent(this.listener);
    this.listener = 0;
  }

  private async unpack(name: string, base: string): Promise<void> {
    const file = fs.openSync(base + '/' + name, fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
    try {
      fs.writeSync(file.fd, this.context.resourceManager.getRawFileContentSync(name).buffer);
    } finally {
      fs.closeSync(file);
    }
    await zlib.decompressFile(base + '/' + name, base, {});
  }

  private append(path: string, message: string): void {
    const file = fs.openSync(path, fs.OpenMode.APPEND | fs.OpenMode.WRITE_ONLY);
    try {
      fs.writeSync(file.fd, message + '\n');
    } finally {
      fs.closeSync(file);
    }
  }

  private launchGtk(session: HgsSession, base: string): void {
    // Fontconfig 需要展开后的绝对字体目录。
    const config = base + '/hgs-gtk-fonts.conf';
    const file = fs.openSync(config, fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
    try {
      fs.writeSync(file.fd, '<?xml version="1.0"?><fontconfig><dir>' + base + '/fonts</dir></fontconfig>');
    } finally {
      fs.closeSync(file);
    }
    const env = session.clientEnvironment('wayland').set;
    const pid = probe.launch([env['XDG_RUNTIME_DIR'], env['WAYLAND_DISPLAY'], base + '/hgs-matrix-gtk.log',
      base + '/hgs-x11/xkb', config].join('\n'), 'wayland');
    if (pid <= 0) throw new Error('GTK probe launch failed: ' + pid);
  }

  private launchXlib(session: HgsSession, base: string): void {
    const env = session.clientEnvironment('x11').set;
    const pid = probe.launch([env['DISPLAY'], env['XAUTHORITY'], base + '/hgs-matrix-x11.log',
      'HGS X11 matrix'].join('\n'));
    if (pid <= 0) throw new Error('Xlib probe launch failed: ' + pid);
  }
}
