import { Component } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { Title } from '@angular/platform-browser';
import { Router, TitleStrategy, provideRouter } from '@angular/router';
import { describe, expect, it } from 'vitest';
import { AppTitleStrategy } from './app-title.strategy';

@Component({ template: '' })
class BlankPage {}

/** Every page title ends with the app name; a page without a title shows the app name alone. */
describe('AppTitleStrategy', () => {
  it('appends the app name to a page title', () => {
    expect(AppTitleStrategy.compose('Modules · Admin')).toBe('Modules · Admin · FPV Radio Map');
  });

  it('uses the app name alone for a missing or blank page title', () => {
    expect(AppTitleStrategy.compose(undefined)).toBe('FPV Radio Map');
    expect(AppTitleStrategy.compose('  ')).toBe('FPV Radio Map');
  });

  it('sets the document title on navigation', async () => {
    TestBed.configureTestingModule({
      providers: [
        provideRouter([
          { path: '', component: BlankPage },
          { path: 'mission/:id', component: BlankPage, title: 'Mission' },
        ]),
        { provide: TitleStrategy, useClass: AppTitleStrategy },
      ],
    });
    const router = TestBed.inject(Router);
    const title = TestBed.inject(Title);

    await router.navigateByUrl('/mission/m-1');
    expect(title.getTitle()).toBe('Mission · FPV Radio Map');

    await router.navigateByUrl('/');
    expect(title.getTitle()).toBe('FPV Radio Map');
  });
});
