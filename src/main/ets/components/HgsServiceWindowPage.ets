import { HgsWindowSurface } from './HgsWindowSurface';

/** Content for the default service host's X11 windows. */
@Component
export struct HgsServiceWindowPage {
  @LocalStorageProp('hgsWindowId') windowId: string = '';
  @LocalStorageProp('hgsEpoch') epoch: string = '';

  build() {
    Stack() {
      if (this.windowId && this.epoch) {
        HgsWindowSurface({ windowId: this.windowId, epoch: this.epoch, interactive: true })
      }
    }.width('100%').height('100%')
  }
}
