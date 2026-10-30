'use strict';
/*
 * sw2-sdk extensions: GPUBrew packages, the services mailbox and the package
 * manager.  Offline; uses temp directories and file:// catalogs.
 *
 * Run: node tests/host/sw2.test.cjs
 */
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const crypto = require('node:crypto');
const assert = require('node:assert');

const ROOT = path.resolve(__dirname, '..', '..');
const {loadPackage, scan, buildCatalog} = require(path.join(ROOT, 'host/lib/packages.cjs'));
const pkgmgr = require(path.join(ROOT, 'host/lib/pkgmgr.cjs'));
const {Services, freshBlock, OP, DATA} = require(path.join(ROOT, 'host/lib/services.cjs'));

const tests = [];
const test = (name, fn) => tests.push({name, fn});
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'sw2-test-'));
const quiet = {info() {}, warn() {}, error() {}};
const SPIRV = (() => { const b = Buffer.alloc(20); b.writeUInt32LE(0x07230203, 0); return b; })();

/* Minimal zip writer (deflate) for fixtures. */
function zip(files) {
  const locals = [], central = [];
  let offset = 0;
  for (const [name, content] of Object.entries(files)) {
    const data = Buffer.from(content);
    const packed = zlib.deflateRawSync(data);
    const nameBytes = Buffer.from(name);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(8, 8);
    local.writeUInt32LE(packed.length, 18); local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBytes.length, 26);
    locals.push(local, nameBytes, packed);
    const entry = Buffer.alloc(46);
    entry.writeUInt32LE(0x02014b50, 0); entry.writeUInt16LE(8, 10);
    entry.writeUInt32LE(packed.length, 20); entry.writeUInt32LE(data.length, 24);
    entry.writeUInt16LE(nameBytes.length, 28); entry.writeUInt32LE(offset, 42);
    central.push(entry, nameBytes);
    offset += 30 + nameBytes.length + packed.length;
  }
  const dir = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(Object.keys(files).length, 8); end.writeUInt16LE(Object.keys(files).length, 10);
  end.writeUInt32LE(dir.length, 12); end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, dir, end]);
}

/* A fake built runner so GPUBrew packages resolve without a cross compiler. */
function fakeRunner() {
  const dir = tmp();
  const code = Buffer.alloc(4096, 0x1f);
  fs.mkdirSync(path.join(dir, 'build'));
  fs.writeFileSync(path.join(dir, 'build/code.bin'), code);
  fs.writeFileSync(path.join(dir, 'build/report.json'), JSON.stringify({
    abi: 3, codeBase: '0x9000000', dataBase: '0xb000000', entry: '0x9000000',
    codeBytes: code.length, dataRegionBytes: 4096,
    codeSha256: crypto.createHash('sha256').update(code).digest('hex')}));
  process.env.SW2_GPUBREW_RUNNER = dir;
  return dir;
}

function gpubrewApp(root, id = 'shader-demo', extra = {}) {
  const dir = path.join(root, id);
  fs.mkdirSync(path.join(dir, 'assets'), {recursive: true});
  for (const stage of ['app.vert.spv', 'app.frag.spv', 'app.comp.spv'])
    fs.writeFileSync(path.join(dir, stage), SPIRV);
  fs.writeFileSync(path.join(dir, 'assets/state.bin'), Buffer.alloc(1024, 1));
  fs.writeFileSync(path.join(dir, 'assets/text.bin'), Buffer.alloc(64, 2));
  fs.writeFileSync(path.join(dir, 'app.json'), JSON.stringify({
    format: 'hblauncher.gpu-app', abi: 1, id, name: 'Shader Demo', version: '1.0.0',
    initialState: 'assets/state.bin', data: 'assets/text.bin',
    shaders: {vertex: 'app.vert.spv', fragment: 'app.frag.spv', compute: 'app.comp.spv'},
    ...extra}));
  return dir;
}

test('GPUBrew app.json loads as a runner launch with named assets', () => {
  fakeRunner();
  const pkg = loadPackage(gpubrewApp(tmp()));
  assert.strictEqual(pkg.manifest.kind, 'gpubrew');
  assert.strictEqual(pkg.manifest.abi, 3);
  const names = pkg.assets.map(a => a.name);
  assert.deepStrictEqual(names, ['gpubrew/desc.bin', 'gpubrew/vertex.spv', 'gpubrew/fragment.spv',
    'gpubrew/compute.spv', 'gpubrew/state.bin', 'gpubrew/data.bin']);
  const desc = pkg.assets[0].bytes;
  assert.strictEqual(desc.readUInt32LE(0), 0x31574247);
  assert.strictEqual(desc.readUInt32LE(12), 8 * 1024 * 1024);
  assert(pkg.code.length > 0, 'runner code attached');
});

test('GPUBrew computeSequence and private memory are carried', () => {
  fakeRunner();
  const root = tmp();
  const dir = gpubrewApp(root, 'seq', {computeSequence: {repeat: 4, begin: 'b.spv', after: 'a.spv'},
    memory: {bytes: 8192, initial: 'assets/mem.bin', encoding: 'deflate'}});
  fs.writeFileSync(path.join(dir, 'b.spv'), SPIRV);
  fs.writeFileSync(path.join(dir, 'a.spv'), SPIRV);
  fs.writeFileSync(path.join(dir, 'assets/mem.bin'), zlib.deflateSync(Buffer.alloc(100, 7)));
  const pkg = loadPackage(dir);
  const desc = pkg.assets[0].bytes;
  assert.strictEqual(desc.readUInt32LE(4), 4);
  assert.strictEqual(desc.readUInt32LE(8), 8192);
  assert.strictEqual(pkg.assets.find(a => a.name === 'gpubrew/memory.bin').bytes.length, 100);
});

test('GPUBrew packages reject non-SPIR-V shaders', () => {
  fakeRunner();
  const dir = gpubrewApp(tmp(), 'bad');
  fs.writeFileSync(path.join(dir, 'app.comp.spv'), 'nope');
  assert.throws(() => loadPackage(dir), /not SPIR-V|larger|must/);
});

test('scan lists native and GPUBrew together with kind code 3', () => {
  fakeRunner();
  const root = tmp();
  fs.cpSync(path.join(ROOT, 'apps/hello-cpu'), path.join(root, 'hello-cpu'), {recursive: true});
  gpubrewApp(root);
  const {packages} = scan(root);
  const kinds = Object.fromEntries(packages.map(p => [p.manifest.id, p.manifest.kind]));
  assert.strictEqual(kinds['shader-demo'], 'gpubrew');
  const catalog = buildCatalog(packages);
  const gpubrewIndex = packages.findIndex(p => p.manifest.kind === 'gpubrew');
  assert.strictEqual(catalog.block.readUInt32LE(32 + gpubrewIndex * 32 + 12), 3);
});

function catalogFixture() {
  const root = tmp();
  const artifact = zip({'zipped/app.json': JSON.stringify({
    format: 'hblauncher.gpu-app', abi: 1, id: 'zipped', name: 'Zipped', version: '2.0.0',
    shaders: {vertex: 'v.spv', fragment: 'f.spv', compute: 'c.spv'}}),
  'zipped/v.spv': SPIRV, 'zipped/f.spv': SPIRV, 'zipped/c.spv': SPIRV});
  const artifactFile = path.join(root, 'zipped.zip');
  fs.writeFileSync(artifactFile, artifact);
  const index = {format: 'sw2-catalog/1', apps: [{id: 'zipped', name: 'Zipped', kind: 'gpubrew',
    version: '2.0.0', author: 'test', category: 'demo', description: 'zip fixture',
    artifact_url: 'file://' + artifactFile, size: artifact.length,
    sha256: crypto.createHash('sha256').update(artifact).digest('hex')}]};
  const indexFile = path.join(root, 'index.json');
  fs.writeFileSync(indexFile, JSON.stringify(index));
  process.env.SW2_CACHE = path.join(root, 'cache');
  return {root, index, url: 'file://' + indexFile};
}

test('pkgmgr installs, reports updates and removes only managed apps', async () => {
  fakeRunner();
  const {root, url} = catalogFixture();
  const apps = path.join(root, 'apps');
  const index = await pkgmgr.loadCatalog({url, refresh: true});
  await pkgmgr.install(apps, index.apps[0], {validate: loadPackage});
  assert(fs.existsSync(path.join(apps, 'zipped/app.json')));
  assert(fs.existsSync(path.join(apps, 'zipped', pkgmgr.MARKER)));
  let row = pkgmgr.status(index, apps)[0];
  assert.strictEqual(row.installed, '2.0.0');
  assert.strictEqual(row.update, false);
  const newer = {...index, apps: [{...index.apps[0], version: '2.1.0'}]};
  assert.strictEqual(pkgmgr.status(newer, apps)[0].update, true);
  pkgmgr.remove(apps, 'zipped');
  assert(!fs.existsSync(path.join(apps, 'zipped')));

  gpubrewApp(apps, 'dev-app');
  assert.throws(() => pkgmgr.remove(apps, 'dev-app'), /not installed by sw2/);
});

test('pkgmgr rejects a tampered artifact', async () => {
  const {root, index} = catalogFixture();
  const bad = {...index.apps[0], sha256: '0'.repeat(64)};
  await assert.rejects(pkgmgr.install(path.join(root, 'apps'), bad), /sha256 mismatch/);
  assert(!fs.existsSync(path.join(root, 'apps/zipped')));
});

test('unzip keeps empty deflated files (native build/data.bin)', () => {
  const files = pkgmgr.unzip(zip({'app/manifest.json': '{}', 'app/build/data.bin': ''}));
  const empty = files.find(f => f.name === 'app/build/data.bin');
  assert(empty && empty.bytes.length === 0);
});

test('unzip refuses path traversal', () => {
  assert.throws(() => pkgmgr.unzip(zip({'../evil': 'x'})), /unsafe path/);
});

test('services mailbox: catalog page, install, denied op', async () => {
  fakeRunner();
  const {root, url} = catalogFixture();
  const apps = path.join(root, 'apps');
  const services = new Services({appsDir: apps, log: quiet, validate: loadPackage, catalogUrl: url});

  const block = freshBlock();
  block.writeUInt32LE(OP.REFRESH, 8); block.writeUInt32LE(1, 12);
  await services.handle(block, ['store']);
  assert.strictEqual(block.readInt32LE(16), 0);
  assert.strictEqual(block.readUInt32LE(32), 1);      /* total */
  assert.strictEqual(block.readUInt32LE(36), 1);      /* rows in page */
  assert.strictEqual(block.readUInt32LE(40), 1);      /* completed == sequence */
  assert.strictEqual(block.subarray(DATA, DATA + 6).toString(), 'zipped');
  assert.strictEqual(block.readUInt32LE(DATA + 356) & 4, 4); /* GPUBREW flag */

  block.writeUInt32LE(OP.INSTALL, 8); block.writeUInt32LE(2, 12);
  block.write('zipped\0', DATA); block.writeUInt32LE(7, 20);
  await services.handle(block, ['store']);
  assert.strictEqual(block.readInt32LE(16), 0, block.subarray(DATA, DATA + 200).toString());
  assert(services.changed);
  assert(fs.existsSync(path.join(apps, 'zipped/app.json')));

  block.writeUInt32LE(OP.REMOVE, 8); block.writeUInt32LE(3, 12);
  await services.handle(block, ['info']);
  assert.strictEqual(block.readInt32LE(16), -1);
});

(async () => {
  let failed = 0;
  for (const {name, fn} of tests) {
    try { await fn(); console.log('ok   ' + name); }
    catch (error) { failed++; console.log('FAIL ' + name + '\n     ' + (error.stack || error)); }
  }
  console.log(failed ? `${failed} failed` : `all ${tests.length} passed`);
  process.exit(failed ? 1 : 0);
})();
