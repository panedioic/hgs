import hgsNapi from 'libhgs.so';
import { HgsServiceOptions, HgsServiceSnapshot, HgsClientEnvironment,
  HgsServiceWindow, HgsServiceEvent, HgsServiceInput, HgsEndpoint, HgsServiceError } from 'libhgs.so';

export class HgsStartError extends Error {
  readonly code: string;
  readonly stage: string;
  constructor(failure: HgsServiceError) {
    super(`${failure.code} (${failure.stage}): ${failure.message}`);
    this.code = failure.code;
    this.stage = failure.stage;
  }
}

/** A session owns no process: clients are launched by the host with explicit env. */
export class HgsSession {
  readonly epoch: string;
  readonly endpoints: HgsEndpoint[];

  constructor(snapshot: HgsServiceSnapshot) {
    this.epoch = snapshot.epoch;
    this.endpoints = snapshot.endpoints;
  }

  clientEnvironment(protocol: 'wayland' | 'x11'): HgsClientEnvironment {
    return hgsNapi.serviceClientEnvironment(protocol, this.epoch);
  }

  windows(): HgsServiceWindow[] {
    return hgsNapi.serviceWindows(this.epoch);
  }

  sendInput(windowId: string, input: HgsServiceInput): boolean {
    const current = hgsNapi.serviceInspect();
    if (current.epoch !== this.epoch || current.state !== 'ready') return false;
    return hgsNapi.serviceSendInput(windowId, input, this.epoch);
  }
}

/** One service per process; v1 startServer shares the same native runtime. */
export class HgsService {
  private static instance: HgsService | null = null;
  private listeners: Map<number, (event: HgsServiceEvent) => void> = new Map();
  private nextListener: number = 0;
  private lifecycle: Promise<void> = Promise.resolve();

  private constructor() {
    hgsNapi.setServiceEventCallback((event: HgsServiceEvent): void => {
      // Copy listeners so unsubscription during delivery is well defined.
      const callbacks: ((event: HgsServiceEvent) => void)[] = [];
      this.listeners.forEach((listener: (event: HgsServiceEvent) => void): void => { callbacks.push(listener); });
      callbacks.forEach((listener: (event: HgsServiceEvent) => void): void => {
        try { listener(event); } catch (error) { hgsNapi.LOGE(`HGS event listener failed: ${String(error)}`); }
      });
    });
  }

  static create(): HgsService {
    if (!HgsService.instance) HgsService.instance = new HgsService();
    return HgsService.instance;
  }

  /** Resolves once the listener and logical output exist; no canvas/client required. */
  async start(options: HgsServiceOptions): Promise<HgsSession> {
    const pending = this.lifecycle.then(async (): Promise<HgsSession> => {
      const result = await hgsNapi.serviceStartAsync(options);
      if (!result.ok) throw new HgsStartError(result.error);
      return new HgsSession(result.session);
    });
    this.lifecycle = pending.then((): void => {}, (): void => {});
    return pending;
  }

  /** Completes after the backend event loop and owned clients have stopped. */
  async stop(): Promise<void> {
    const pending = this.lifecycle.then(async (): Promise<void> => {
      await hgsNapi.serviceStopAsync();
    });
    this.lifecycle = pending.then((): void => {}, (): void => {});
    return pending;
  }

  inspect(): HgsServiceSnapshot { return hgsNapi.serviceInspect(); }

  onEvent(listener: (event: HgsServiceEvent) => void): number {
    const token = ++this.nextListener;
    this.listeners.set(token, listener);
    return token;
  }

  offEvent(token: number): void { this.listeners.delete(token); }
}
