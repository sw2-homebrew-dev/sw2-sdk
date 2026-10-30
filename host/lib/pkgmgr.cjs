'use strict';
/*
 * pkgmgr.cjs - catalog client and package installer.
 *
 * Shared by the on-console store (through the services mailbox) and the
 * desktop fallback CLI (sw2-store).  Everything happens on the workstation:
 * the console never sees a zip, only the validated apps/<id> directory.
 *
 * Install is all or nothing: download -> sha256 -> unpack into a staging
 * directory -> loadPackage() -> swap into apps/<id>.  The previous copy is
 * kept as apps/.<id>.old until the swap succeeds.  Only directories carrying
 * our install marker are ever removed, so local development apps are safe.
 */
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const crypto = require('node:crypto');

const DEFAULT_CATALOG = process.env.SW2_CATALOG_URL ||
  'https://sw2-homebrew-dev.github.io/sw2-homebrew-catalog/index.json';
const MARKER = '.sw2-install.json';
const ID_PATTERN = /^[a-z0-9][a-z0-9._-]{0,63}$/;
const MAX_ARTIFACT = 256 * 1024 * 1024;
const MAX_ENTRIES = 4096;

class PkgError extends Error {
  constructor(message, code) { super(message); this.code = code; }
}

function cacheDir() {
  const base = process.env.SW2_CACHE ||
    path.join(process.env.XDG_CACHE_HOME || path.join(os.homedir(), '.cache'), 'sw2');
  fs.mkdirSync(base, {recursive: true});
  return base;
}

async function fetchBytes(url, limit = MAX_ARTIFACT) {
  if (url.startsWith('file://')) return fs.readFileSync(new URL(url));
  if (!/^https?:\/\//.test(url)) return fs.readFileSync(url);
  let response;
  try {
    response = await fetch(url, {redirect: 'follow',
      headers: {'user-agent': 'sw2-pkgmgr/1'}});
  } catch (error) {
    throw new PkgError(`download failed: ${url}: ${error.message}`, 'network');
  }
  if (!response.ok) throw new PkgError(`download failed: ${url}: HTTP ${response.status}`, 'network');
  const declared = Number(response.headers.get('content-length') || 0);
  if (declared > limit) throw new PkgError(`${url} is larger than ${limit} bytes`, 'integrity');
  const bytes = Buffer.from(await response.arrayBuffer());
  if (bytes.length > limit) throw new PkgError(`${url} is larger than ${limit} bytes`, 'integrity');
  return bytes;
}

/* --- catalog ---------------------------------------------------------- */

function validateIndex(index) {
  if (!index || index.format !== 'sw2-catalog/1' || !Array.isArray(index.apps))
    throw new PkgError('catalog index is not sw2-catalog/1', 'integrity');
  const apps = [];
  for (const app of index.apps) {
    if (!app || !ID_PATTERN.test(app.id || '')) continue;
    if (!/^[0-9a-f]{64}$/.test(app.sha256 || '') || typeof app.artifact_url !== 'string') continue;
    apps.push(app);
  }
  return {...index, apps};
}

async function loadCatalog({url = DEFAULT_CATALOG, refresh = false} = {}) {
  const cached = path.join(cacheDir(), 'catalog.json');
  if (!refresh && fs.existsSync(cached)) {
    try { return validateIndex(JSON.parse(fs.readFileSync(cached, 'utf8'))); }
    catch { /* fall through to a fresh download */ }
  }
  const index = validateIndex(JSON.parse((await fetchBytes(url, 32 * 1024 * 1024)).toString('utf8')));
  fs.writeFileSync(cached, JSON.stringify(index));
  return index;
}

/* --- installed state -------------------------------------------------- */

function readManifest(directory) {
  for (const name of ['manifest.json', 'app.json']) {
    const file = path.join(directory, name);
    if (fs.existsSync(file)) {
      try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return null; }
    }
  }
  return null;
}

function installed(appsDir) {
  const result = new Map();
  if (!fs.existsSync(appsDir)) return result;
  for (const entry of fs.readdirSync(appsDir)) {
    if (entry.startsWith('.')) continue;
    const directory = path.join(appsDir, entry);
    if (!fs.statSync(directory).isDirectory()) continue;
    const manifest = readManifest(directory);
    if (!manifest || !manifest.id) continue;
    const markerFile = path.join(directory, MARKER);
    const marker = fs.existsSync(markerFile) ? JSON.parse(fs.readFileSync(markerFile, 'utf8')) : null;
    result.set(manifest.id, {id: manifest.id, version: String(manifest.version || ''),
      directory, managed: !!marker, marker});
  }
  return result;
}

function compareVersions(a, b) {
  const parts = v => String(v).split(/[.+-]/).map(p => /^\d+$/.test(p) ? Number(p) : p);
  const x = parts(a), y = parts(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const p = x[i] ?? 0, q = y[i] ?? 0;
    if (p === q) continue;
    if (typeof p === 'number' && typeof q === 'number') return p < q ? -1 : 1;
    return String(p) < String(q) ? -1 : 1;
  }
  return 0;
}

/* Catalog rows merged with what is on disk. */
function status(index, appsDir) {
  const local = installed(appsDir);
  return index.apps.map(app => {
    const have = local.get(app.id);
    return {...app, installed: have ? have.version : null,
      managed: have ? have.managed : false,
      update: !!have && compareVersions(have.version, app.version) < 0};
  });
}

/* --- zip (stored + deflate, no zip64) --------------------------------- */

function unzip(buffer) {
  let eocd = -1;
  for (let i = buffer.length - 22; i >= Math.max(0, buffer.length - 65557); i--)
    if (buffer.readUInt32LE(i) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new PkgError('artifact is not a zip file', 'integrity');
  const count = buffer.readUInt16LE(eocd + 10);
  let at = buffer.readUInt32LE(eocd + 16);
  if (count > MAX_ENTRIES) throw new PkgError('zip has too many entries', 'integrity');
  const files = [];
  let total = 0;
  for (let n = 0; n < count; n++) {
    if (buffer.readUInt32LE(at) !== 0x02014b50) throw new PkgError('corrupt zip directory', 'integrity');
    const method = buffer.readUInt16LE(at + 10);
    const compressed = buffer.readUInt32LE(at + 20);
    const size = buffer.readUInt32LE(at + 24);
    const nameLength = buffer.readUInt16LE(at + 28);
    const extraLength = buffer.readUInt16LE(at + 30);
    const commentLength = buffer.readUInt16LE(at + 32);
    const local = buffer.readUInt32LE(at + 42);
    const name = buffer.subarray(at + 46, at + 46 + nameLength).toString('utf8').replace(/\\/g, '/');
    at += 46 + nameLength + extraLength + commentLength;
    if (name.endsWith('/')) continue;
    if (name.startsWith('/') || name.split('/').some(p => p === '..' || p === ''))
      throw new PkgError(`unsafe path in zip: ${name}`, 'integrity');
    if (buffer.readUInt32LE(local) !== 0x04034b50) throw new PkgError('corrupt zip entry', 'integrity');
    const start = local + 30 + buffer.readUInt16LE(local + 26) + buffer.readUInt16LE(local + 28);
    const raw = buffer.subarray(start, start + compressed);
    total += size;
    if (total > MAX_ARTIFACT) throw new PkgError('zip expands beyond the size limit', 'integrity');
    let bytes;
    if (method === 0) bytes = Buffer.from(raw);
    /* zlib rejects maxOutputLength 0, and empty files (an app's empty
       build/data.bin) are common. */
    else if (method === 8) bytes = size ? zlib.inflateRawSync(raw, {maxOutputLength: size}) : Buffer.alloc(0);
    else throw new PkgError(`unsupported zip method ${method} for ${name}`, 'integrity');
    if (bytes.length !== size) throw new PkgError(`size mismatch for ${name}`, 'integrity');
    files.push({name, bytes});
  }
  return files;
}

/* Packages may be zipped flat or inside one top-level folder. */
function packageRoot(files) {
  if (files.some(f => f.name === 'manifest.json' || f.name === 'app.json')) return '';
  const tops = new Set(files.map(f => f.name.split('/')[0]));
  if (tops.size === 1) {
    const [top] = tops;
    if (files.some(f => f.name === `${top}/manifest.json` || f.name === `${top}/app.json`))
      return top + '/';
  }
  throw new PkgError('zip has no manifest.json or app.json at its root', 'integrity');
}

/* --- install / remove ------------------------------------------------- */

async function install(appsDir, app, {validate, log = () => {}} = {}) {
  if (!ID_PATTERN.test(app.id || '')) throw new PkgError('invalid app id', 'unknown');
  const local = installed(appsDir).get(app.id);
  if (local && !local.managed)
    throw new PkgError(`${app.id} exists in ${local.directory} and was not installed by sw2; ` +
      'remove it by hand first', 'io');

  log(`downloading ${app.artifact_url}`);
  const bytes = await fetchBytes(app.artifact_url);
  const digest = crypto.createHash('sha256').update(bytes).digest('hex');
  if (digest !== app.sha256)
    throw new PkgError(`sha256 mismatch for ${app.id}: expected ${app.sha256}, got ${digest}`, 'integrity');

  const files = unzip(bytes);
  const root = packageRoot(files);
  fs.mkdirSync(appsDir, {recursive: true});
  const staging = fs.mkdtempSync(path.join(appsDir, `.${app.id}.staging-`));
  try {
    for (const file of files) {
      if (!file.name.startsWith(root)) continue;
      const target = path.join(staging, file.name.slice(root.length));
      if (!target.startsWith(staging + path.sep)) throw new PkgError('unsafe zip path', 'integrity');
      fs.mkdirSync(path.dirname(target), {recursive: true});
      fs.writeFileSync(target, file.bytes);
    }
    const manifest = readManifest(staging);
    if (!manifest || manifest.id !== app.id)
      throw new PkgError(`package id ${manifest && manifest.id} does not match catalog id ${app.id}`, 'integrity');
    if (validate) {
      try { validate(staging); }
      catch (error) { throw new PkgError(`package rejected: ${error.message}`, 'integrity'); }
    }
    fs.writeFileSync(path.join(staging, MARKER), JSON.stringify({
      id: app.id, version: app.version, sha256: app.sha256,
      source: app.artifact_url, installed: new Date().toISOString()}, null, 2));

    const finalDir = local ? local.directory : path.join(appsDir, app.id);
    const backup = path.join(appsDir, `.${app.id}.old`);
    fs.rmSync(backup, {recursive: true, force: true});
    if (fs.existsSync(finalDir)) fs.renameSync(finalDir, backup);
    try {
      fs.renameSync(staging, finalDir);
    } catch (error) {
      if (fs.existsSync(backup)) fs.renameSync(backup, finalDir);
      throw new PkgError(`could not place ${app.id}: ${error.message}`, 'io');
    }
    fs.rmSync(backup, {recursive: true, force: true});
    log(`installed ${app.id} ${app.version}`);
    return {id: app.id, version: app.version, directory: finalDir, updated: !!local};
  } finally {
    fs.rmSync(staging, {recursive: true, force: true});
  }
}

function remove(appsDir, id) {
  const local = installed(appsDir).get(id);
  if (!local) throw new PkgError(`${id} is not installed`, 'unknown');
  if (!local.managed)
    throw new PkgError(`${id} was not installed by sw2; refusing to delete ${local.directory}`, 'io');
  fs.rmSync(local.directory, {recursive: true, force: true});
  return {id, directory: local.directory};
}

module.exports = {DEFAULT_CATALOG, MARKER, PkgError, loadCatalog, validateIndex, installed,
  status, compareVersions, unzip, install, remove, fetchBytes};
