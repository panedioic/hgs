import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = '[HGS]';

// 临时导出函数 (helloworld 第二步): 由 entry 调用, 拉起 hgs 的 HgsWindowAbility。
// 正式迁移后, 该入口演进为 hgs 窗口管理的公开 API。
// bundleName 从调用方 context 动态解析 (HAR 不硬编码宿主包名);
// 本 API 版本上 abilityName-only 的 want 不按调用方 bundle 解析,
// 会走 free-install 路径报 16000001, 必须显式携带 bundleName。
export function openHgsWindow(context: common.UIAbilityContext): void {
  const bundleName: string = context.abilityInfo?.bundleName
    ?? context.applicationInfo?.name ?? '';
  hilog.info(DOMAIN, TAG, 'openHgsWindow bundle=%{public}s', bundleName);
  context.startAbility({
    bundleName: bundleName,
    abilityName: 'HgsWindowAbility'
  })
    .then(() => {
      hilog.info(DOMAIN, TAG, 'HgsWindowAbility started');
    })
    .catch((err: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'start HgsWindowAbility failed: %{public}d %{public}s', err.code, err.message);
    });
}
