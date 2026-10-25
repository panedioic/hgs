import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const DOMAIN = 0x0000;
const TAG = '[HGS-Ability]';

// hgs 提供的窗口 Ability (helloworld 第二步)。
// 载入宿主注册的占位页 (pages/HgsWindow, 由宿主 main_pages.json 注册),
// 页面内 EmptyEglView 用 hgs 原生空 EGL 渲染器上屏。
// 正式迁移后, 页面与窗口管理逻辑逐步收进 hgs。
export class HgsWindowAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/HgsWindow', (err: BusinessError) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, 'page loaded');
    });
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onDestroy');
  }
}
