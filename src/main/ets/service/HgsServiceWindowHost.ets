import { display, window } from '@kit.ArkUI';
import hgs, { HgsServiceEvent, HgsServiceWindow } from 'libhgs.so';
import { HgsService, HgsSession } from '../api/HgsService';

class Entry {
  snapshot: HgsServiceWindow;
  host: window.Window | null = null;
  opening: boolean = false;
  shown: boolean = false;
  scale: number = 1;
  private layout: Promise<void> = Promise.resolve();

  constructor(snapshot: HgsServiceWindow) {
    this.snapshot = snapshot;
  }

  enqueueLayout(task: () => Promise<void>): void {
    this.layout = this.layout.then(task).catch((error: Error): void => {
      hgs.LOGW(`HGS X11 layout ${this.snapshot.id}: ${String(error)}`);
    });
  }
}

/** Adapts service WindowIds to OHOS subwindows while the service is running. */
export class HgsServiceWindowHost {
  private entries: Map<string, Entry> = new Map();
  private listener: number = 0;
  private nextHost: number = 0;
  private session: HgsSession;
  private stage: window.WindowStage;
  private pageUrl: string;
  private windowScale: number;
  private windowShownListener: ((window: HgsServiceWindow) => void) | null = null;

  constructor(session: HgsSession, stage: window.WindowStage, pageUrl: string, windowScale: number) {
    this.session = session;
    this.stage = stage;
    this.pageUrl = pageUrl;
    this.windowScale = windowScale;
  }

  setWindowShownListener(listener: ((window: HgsServiceWindow) => void) | null): void {
    this.windowShownListener = listener;
  }

  isWindowShown(id: string): boolean {
    return this.entries.get(id)?.shown ?? false;
  }

  start(): void {
    this.listener = HgsService.create().onEvent((event: HgsServiceEvent): void => {
      if (event.epoch !== this.session.epoch) return;
      if (event.state === 'stopping' || event.state === 'stopped' ||
        (event.kind === 'backend-disconnected' && event.origin === 'xwayland')) {
        this.stop();
      } else if (event.window.protocol === 'x11') {
        if (event.kind === 'destroyed') this.remove(event.window.id);
        else if (event.kind === 'created' || event.kind === 'changed') this.update(event.window);
        else if (event.kind === 'close-requested') this.session.controlWindow(event.window.id, { kind: 'close' });
      }
    });
    this.session.windows().forEach((snapshot: HgsServiceWindow): void => this.update(snapshot));
  }

  stop(): void {
    if (this.listener) HgsService.create().offEvent(this.listener);
    this.listener = 0;
    const entries = this.entries;
    this.entries = new Map();
    entries.forEach((entry: Entry): void => {
      if (entry.host && !entry.opening) void this.destroy(entry.host);
    });
  }

  private remove(id: string): void {
    const entry = this.entries.get(id);
    if (!entry) return;
    this.entries.delete(id);
    this.entries.forEach((child: Entry): void => {
      if (child.snapshot.parentId === id) this.remove(child.snapshot.id);
    });
    if (entry.host && !entry.opening) void this.destroy(entry.host);
  }

  private async destroy(host: window.Window): Promise<void> {
    try { await host.destroyWindow(); } catch (_) { /* Already gone with its parent. */ }
  }

  private update(snapshot: HgsServiceWindow): void {
    if (snapshot.protocol !== 'x11') return;
    const id = snapshot.id;
    if (!snapshot.visible || snapshot.width <= 0 || snapshot.height <= 0) {
      this.remove(id);
      return;
    }
    let entry = this.entries.get(id);
    if (entry && (entry.snapshot.parentId !== snapshot.parentId || entry.snapshot.modal !== snapshot.modal ||
      entry.snapshot.role !== snapshot.role)) {
      this.remove(id);
      entry = undefined;
    }
    if (!entry) {
      entry = new Entry(snapshot);
      this.entries.set(id, entry);
    } else {
      const before = entry.snapshot;
      entry.snapshot = snapshot;
      if (entry.shown && (before.x !== snapshot.x || before.y !== snapshot.y ||
        before.width !== snapshot.width || before.height !== snapshot.height || before.title !== snapshot.title)) {
        const changed = entry;
        changed.enqueueLayout(async (): Promise<void> => this.layout(changed));
      }
    }
    this.openIfReady(entry);
  }

  private openIfReady(entry: Entry): void {
    if (entry.host || entry.opening) return;
    const snapshot = entry.snapshot;
    const parent = this.entries.get(snapshot.parentId);
    if (snapshot.parentId && !parent?.shown) return;
    entry.opening = true;
    void this.open(entry, parent?.host ?? null);
  }

  private async layout(entry: Entry): Promise<void> {
    const host = entry.host;
    const snapshot = entry.snapshot;
    if (!host || this.entries.get(snapshot.id) !== entry) return;
    const screen = display.getDefaultDisplaySync();
    const parent = this.entries.get(snapshot.parentId);
    entry.scale = Math.min(parent?.scale ?? this.windowScale,
      screen.width / snapshot.width, screen.height / snapshot.height);
    const width = Math.max(1, Math.round(snapshot.width * entry.scale));
    const height = Math.max(1, Math.round(snapshot.height * entry.scale));
    const rect = host.getWindowProperties().windowRect;
    if (rect.width !== width || rect.height !== height) await host.resize(width, height);
    let x = Math.round(snapshot.x * entry.scale);
    let y = Math.round(snapshot.y * entry.scale);
    if (parent?.host) {
      const anchor = parent.host.getWindowProperties().windowRect;
      x = anchor.left + Math.round((snapshot.x - parent.snapshot.x) * parent.scale);
      y = anchor.top + Math.round((snapshot.y - parent.snapshot.y) * parent.scale);
    }
    await host.moveWindowToGlobal(Math.max(0, Math.min(x, screen.width - width)),
      Math.max(0, Math.min(y, screen.height - height)));
    if (entry.shown && snapshot.role !== 'popup') await host.setWindowTitle(snapshot.title);
  }

  private async open(entry: Entry, parent: window.Window | null): Promise<void> {
    const id = entry.snapshot.id;
    const name = `hgs_${this.session.epoch}_${++this.nextHost}`;
    const popup = entry.snapshot.role === 'popup';
    const options: window.SubWindowOptions = {
      title: entry.snapshot.title, decorEnabled: !popup, isModal: entry.snapshot.modal
    };
    let host: window.Window | null = null;
    try {
      host = parent
        ? await parent.createSubWindowWithOptions(name, options)
        : await this.stage.createSubWindowWithOptions(name, options);
      if (this.entries.get(id) !== entry) return;
      entry.host = host;
      await this.layout(entry);
      if (popup && !entry.snapshot.modal) await host.setWindowFocusable(false);
      if (!popup || entry.snapshot.modal) this.observeHost(entry, host);
      const storage = new LocalStorage();
      storage.setOrCreate<string>('hgsWindowId', id);
      storage.setOrCreate<string>('hgsEpoch', this.session.epoch);
      await host.loadContent(this.pageUrl, storage);
      if (this.entries.get(id) !== entry) return;
      await host.showWindow();
      if (this.entries.get(id) !== entry) return;
      entry.shown = true;
      this.windowShownListener?.(entry.snapshot);
      entry.enqueueLayout(async (): Promise<void> => this.layout(entry));
      this.entries.forEach((child: Entry): void => {
        if (child.snapshot.parentId === id) this.openIfReady(child);
      });
    } catch (error) {
      if (this.entries.get(id) === entry) {
        this.remove(id);
        hgs.LOGE(`HGS X11 host ${id}: ${String(error)}`);
      }
    } finally {
      entry.opening = false;
      if (host && this.entries.get(id) !== entry) await this.destroy(host);
    }
  }

  private observeHost(entry: Entry, host: window.Window): void {
    const id = entry.snapshot.id;
    host.on('windowWillClose', async (): Promise<boolean> =>
      this.entries.get(id) === entry && this.session.controlWindow(id, { kind: 'close' }));
    host.on('windowEvent', (event: window.WindowEventType): void => {
      if (this.entries.get(id) !== entry) return;
      if (event === window.WindowEventType.WINDOW_ACTIVE) this.session.controlWindow(id, { kind: 'focus' });
      else if (event === window.WindowEventType.WINDOW_INACTIVE) this.session.controlWindow(id, { kind: 'blur' });
    });
    host.on('windowRectChange', (event: window.RectChangeOptions): void => {
      if (this.entries.get(id) !== entry) return;
      if (event.reason === window.RectChangeReason.DRAG_END) {
        const width = Math.min(32767, Math.max(1, Math.round(event.rect.width / entry.scale)));
        const height = Math.min(32767, Math.max(1, Math.round(event.rect.height / entry.scale)));
        this.session.controlWindow(id, { kind: 'resize', width: width, height: height });
      }
      this.entries.forEach((child: Entry): void => {
        if (child.snapshot.parentId === id && child.host)
          child.enqueueLayout(async (): Promise<void> => this.layout(child));
      });
    });
    host.enableDrag(true).catch((error: Error): void => hgs.LOGW(`HGS X11 resize ${id}: ${String(error)}`));
  }
}
