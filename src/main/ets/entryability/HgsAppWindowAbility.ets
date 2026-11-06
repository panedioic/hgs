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
import { window, display } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import bundleManager from '@ohos.bundle.bundleManager';
import hgs from 'libhgs.so';
import { HgsWindowHostService } from '../service/HgsWindowHostService';

const TAG = 'HGS_WinAbility';

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
    hgs.LOGI(`[${TAG}] onCreate tl=${this.toplevelId} title=${this.title} ${this.contentW}x${this.contentH}`);
    if (this.toplevelId > 0) {
      hgs.setPendingToplevel(this.toplevelId);
    }
  }

  async onWindowStageCreate(stage: window.WindowStage): Promise<void> {
    const mainWin: window.Window = stage.getMainWindowSync();
    // ── 显示前配置 (loadContent 之前): ──
    // 1. 窗口模式 (可选): floatingOnly=true 时声明 FLOATING-only (stage API,
    //    since 15/20), host 用几何方式自管最大化; 默认 false 走系统默认
    //    模式 + 系统原生 maximize/restore (见 HgsWindowHostOptions 注释)
    // 2. 预设窗口尺寸 (显示前 resize 生效; 显示后再 resize 会被迁移态吞掉)。
    //    这一步同时压制系统的"记住全屏布局、显示时重新施加" — 真机验证
    //    最大化会话后强停重启, 窗口仍以普通态打开
    // 3. 隐藏 OHOS 系统标题栏 (显示后才可调, pre-show 报 1300002;
    //    attachWindow 的重试路径也会再调一次)
    if (HgsWindowHostService.getInstance().floatingOnly()) {
      try {
        await stage.setSupportedWindowModes(
          [bundleManager.SupportWindowMode.FLOATING], true);
      } catch (e) {
        hgs.LOGW(`[${TAG}] setSupportedWindowModes: ${JSON.stringify(e)}`);
      }
    } else {
      hgs.LOGI(`[${TAG}] floatingOnly=false — 系统默认窗口模式`);
    }
    const scale = HgsWindowHostService.getInstance().windowScale();
    if (this.contentW > 0 && this.contentH > 0) {
      try {
        await mainWin.resize(Math.round(this.contentW * scale),
                             Math.round(this.contentH * scale));
        const r = mainWin.getWindowProperties().windowRect;
        hgs.LOGI(`[${TAG}] pre-show resize req=${Math.round(this.contentW * scale)}x${Math.round(this.contentH * scale)} ` +
                 `rect=${r.width}x${r.height}@(${r.left},${r.top}) density=${display.getDefaultDisplaySync().densityPixels}`);
      } catch (e) {
        hgs.LOGW(`[${TAG}] pre-show resize failed: ${JSON.stringify(e)}`);
      }
    }
    try {
      mainWin.setWindowDecorVisible(false);
    } catch (e) {
      hgs.LOGW(`[${TAG}] setWindowDecorVisible(pre-show): ${JSON.stringify(e)}`);
    }
    try {
      await stage.loadContent(HgsWindowHostService.getInstance().pageUrl(), null);
    } catch (e) {
      const err = e as BusinessError;
      hgs.LOGE(`[${TAG}] loadContent failed tl=${this.toplevelId}: ${JSON.stringify(err)}`);
      hgs.notifyWindowClosed(this.toplevelId);
      return;
    }
    try {
      HgsWindowHostService.getInstance().attachWindow(
        this.toplevelId, mainWin, this.context);
    } catch (e) {
      const err = e as BusinessError;
      hgs.LOGE(`[${TAG}] attachWindow failed tl=${this.toplevelId}: ${JSON.stringify(err)}`);
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
