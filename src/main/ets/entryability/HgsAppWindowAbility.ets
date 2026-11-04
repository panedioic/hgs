// hgs/src/main/ets/entryability/HgsAppWindowAbility.ets
//
// 通用窗口 Ability 基类 (阶段 2 W2b)。宿主在 module.json5 注册一个 5 行子类:
//   import { HgsAppWindowAbility } from 'hgs';
//   export default class AppRunnerAbility extends HgsAppWindowAbility {}
// launchType 必须为 multiton (每个 toplevel 一个实例)。
//
// 职责: 读 want 参数 → setPendingToplevel → loadContent(pageUrl) →
// attachWindow(id, mainWindow, context)。窗口的 title/几何/状态后续由
// HgsWindowHostService 经 HgsWindowEvent 驱动。

import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import hgs from 'libhgs.so';
import { HgsWindowHostService } from '../service/HgsWindowHostService';

const TAG = 'HGS_WinAbility';
const DOMAIN = 0x0000;

export default class HgsAppWindowAbility extends UIAbility {
  protected toplevelId: number = 0;
  protected title: string = '';
  protected contentW: number = 0;
  protected contentH: number = 0;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    const params: Record<string, Object> = (want.parameters ?? {}) as Record<string, Object>;
    this.toplevelId = Number(params.toplevelId ?? 0);
    this.title = (params.title as string) ?? '';
    this.contentW = Number(params.w ?? 0);
    this.contentH = Number(params.h ?? 0);
    hilog.info(DOMAIN, TAG, 'onCreate tl=%{public}d title=%{public}s %{public}dx%{public}d',
      this.toplevelId, this.title, this.contentW, this.contentH);
    if (this.toplevelId > 0) {
      hgs.setPendingToplevel(this.toplevelId);
    }
  }

  async onWindowStageCreate(stage: window.WindowStage): Promise<void> {
    try {
      await stage.loadContent(HgsWindowHostService.getInstance().pageUrl(), null);
    } catch (e) {
      const err = e as BusinessError;
      hilog.error(DOMAIN, TAG, 'loadContent failed tl=%{public}d: %{public}s',
        this.toplevelId, JSON.stringify(err));
      hgs.notifyWindowClosed(this.toplevelId);
      return;
    }
    try {
      const mainWin: window.Window = stage.getMainWindowSync();
      HgsWindowHostService.getInstance().attachWindow(
        this.toplevelId, mainWin, this.context);
    } catch (e) {
      const err = e as BusinessError;
      hilog.error(DOMAIN, TAG, 'attachWindow failed tl=%{public}d: %{public}s',
        this.toplevelId, JSON.stringify(err));
      hgs.notifyWindowClosed(this.toplevelId);
    }
  }

  onWindowStageDestroy(): void {
    // 窗口被销毁: 通知 core 回滚并请求客户端关闭 (core 主动销毁时幂等)
    if (this.toplevelId > 0) {
      hgs.closeToplevel(this.toplevelId);
      HgsWindowHostService.getInstance().detachWindow(this.toplevelId);
    }
  }
}
