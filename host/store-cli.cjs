#!/usr/bin/env node
'use strict';
/*
 * store-cli.cjs - desktop store, the fallback for the on-console store.
 *
 *   sw2 store list [--installed|--updates]
 *   sw2 store install <id>...
 *   sw2 store update [id...]        (no ids: every managed app with an update)
 *   sw2 store remove <id>...
 *   sw2 store refresh
 *
 * Environment: SW2_CATALOG_URL (index.json), HBL_APPS (apps directory).
 */
const path = require('node:path');
const pkgmgr = require('./lib/pkgmgr.cjs');
const {loadPackage} = require('./lib/packages.cjs');

const ROOT = path.resolve(__dirname, '..');
const APPS = process.env.HBL_APPS || path.join(ROOT, 'apps');
const say = message => console.log(message);

async function main(argv) {
  const [command = 'list', ...rest] = argv;
  const flags = new Set(rest.filter(a => a.startsWith('--')));
  const ids = rest.filter(a => !a.startsWith('--'));
  const index = await pkgmgr.loadCatalog({refresh: command === 'refresh' || flags.has('--refresh')});
  const rows = pkgmgr.status(index, APPS);
  const byId = new Map(index.apps.map(app => [app.id, app]));

  if (command === 'list' || command === 'refresh') {
    const shown = rows.filter(r => flags.has('--installed') ? r.installed :
      flags.has('--updates') ? r.update : true);
    for (const r of shown.sort((a, b) => a.name.localeCompare(b.name))) {
      const state = r.update ? `update ${r.installed} -> ${r.version}` :
        r.installed ? `installed ${r.installed}` : r.version;
      say(`${r.id.padEnd(28)} ${String(r.kind || 'native').padEnd(8)} ${state.padEnd(24)} ${r.name}`);
    }
    say(`${shown.length} of ${rows.length} apps  (catalog: ${pkgmgr.DEFAULT_CATALOG})`);
    return 0;
  }
  if (command === 'install' || command === 'update') {
    const targets = ids.length ? ids :
      command === 'update' ? rows.filter(r => r.update && r.managed).map(r => r.id) : [];
    if (!targets.length) { say(command === 'update' ? 'everything is up to date' : 'nothing to install'); return 0; }
    let failed = 0;
    for (const id of targets) {
      const app = byId.get(id);
      if (!app) { console.error(`${id}: not in the catalog`); failed++; continue; }
      try { await pkgmgr.install(APPS, app, {validate: loadPackage, log: say}); }
      catch (error) { console.error(`${id}: ${error.message}`); failed++; }
    }
    return failed ? 1 : 0;
  }
  if (command === 'remove') {
    let failed = 0;
    for (const id of ids) {
      try { pkgmgr.remove(APPS, id); say(`removed ${id}`); }
      catch (error) { console.error(error.message); failed++; }
    }
    return failed ? 1 : 0;
  }
  console.error('usage: sw2 store <list|install|update|remove|refresh> [id...]');
  return 2;
}

main(process.argv.slice(2)).then(code => { process.exitCode = code; },
  error => { console.error(error.message); process.exitCode = 1; });
