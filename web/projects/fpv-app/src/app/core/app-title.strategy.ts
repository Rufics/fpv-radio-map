import { Injectable, inject } from '@angular/core';
import { Title } from '@angular/platform-browser';
import { RouterStateSnapshot, TitleStrategy } from '@angular/router';

/** Document titles: the route's `title` first, then the app name, so tabs tell pages apart. */
@Injectable()
export class AppTitleStrategy extends TitleStrategy {
  static readonly APP_NAME = 'FPV Radio Map';
  static readonly SEPARATOR = ' · ';

  private readonly title = inject(Title);

  /** The title for a route title, or the app name alone when the route has none. */
  static compose(pageTitle: string | undefined): string {
    const page = pageTitle?.trim();
    return page
      ? `${page}${AppTitleStrategy.SEPARATOR}${AppTitleStrategy.APP_NAME}`
      : AppTitleStrategy.APP_NAME;
  }

  override updateTitle(snapshot: RouterStateSnapshot): void {
    this.title.setTitle(AppTitleStrategy.compose(this.buildTitle(snapshot)));
  }
}
