// hgs/src/main/ets/components/HgsWindowPage.ets
//
// 通用窗口页 (阶段 2 W2b): HgsAppWindowAbility 的 loadContent 目标。
// 宿主在 main_pages.json 注册一个 5 行包装页 re-export 本组件即可。
//
// 与 BetterSH ClientWindow 的差异: 输入完全走 Native 后端 (inputBackend=2),
// 不注册 onTouch/onMouse/onAxisEvent/onKeyEvent; 渲染器由 native
// OnSurfaceCreated 直接绑定, 不调 createRenderer/attachSurface。

import { HgsSurface } from './HgsSurface';
import hgs from 'libhgs.so';

@Component
export struct HgsWindowPage {
  @State toplevelId: number = 0;

  aboutToAppear(): void {
    // Ability onCreate 已 hgs.setPendingToplevel(id); 页面同步取走 (出队)
    this.toplevelId = hgs.takePendingToplevel() as number;
  }

  build() {
    Column() {
      HgsSurface({
        xcId: 'hgs_app_xc',
        toplevelId: this.toplevelId,
        inputBackend: 2,
        keyboardTarget: this.toplevelId,
        tag: 'HgsWindow',
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
  }
}
