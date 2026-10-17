import { Routes } from '@angular/router';
import { WorkspacePage } from './workspace/workspace.page';

export const routes: Routes = [
  { path: '', component: WorkspacePage, title: 'FPV Radio Map' },
  { path: 'mission/:id', component: WorkspacePage, title: 'Mission · FPV Radio Map' },
  { path: 'm/:role/:token', loadComponent: () => import('./shared/pin-entry.page').then((m) => m.PinEntryPage), title: 'Shared mission · FPV Radio Map' },
  {
    path: 'admin',
    loadComponent: () => import('./admin/admin-shell.component').then((m) => m.AdminShellComponent),
    children: [
      { path: '', pathMatch: 'full', redirectTo: 'modules' },
      { path: 'modules', loadComponent: () => import('./admin/modules.page').then((m) => m.ModulesPage), title: 'Modules · Admin' },
      { path: 'library', loadComponent: () => import('./admin/library.page').then((m) => m.LibraryPage), title: 'Reference library · Admin' },
      { path: 'sources', loadComponent: () => import('./admin/sources.page').then((m) => m.SourcesPage), title: 'Data sources · Admin' },
    ],
  },
  { path: '**', redirectTo: '' },
];
