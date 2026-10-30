'use strict';
/*
 * services.cjs - host side of the hbx_svc mailbox (sdk/include/hbx_svc.h).
 *
 * An app that declares "services" gets a 64 KiB block at ctx->args[5].  When
 * it returns HBX_SVC_CALL the runtime hands the block to handle(), writes the
 * reply back and re-enters the app.  Offsets mirror struct hbx_svc.
 */
const pkgmgr = require('./pkgmgr.cjs');

const MAGIC = 0x43565348;
const VERSION = 1;
const BLOCK_BYTES = 65536;
const SCRATCH = 64;
const DATA = 64 + 16320;
const DATA_BYTES = 49152;
const APP_BYTES = 360;
const CALL = 0x5356;
const SERVICES = new Set(['store', 'info']);

const OP = {INFO: 1, CATALOG: 2, INSTALL: 3, REMOVE: 4, REFRESH: 5};
const NEEDS = {1: 'info', 2: 'store', 3: 'store', 4: 'store', 5: 'store'};
const ERR = {denied: -1, unknown: -2, network: -3, integrity: -4, io: -5};

function freshBlock() {
  const block = Buffer.alloc(BLOCK_BYTES);
  block.writeUInt32LE(MAGIC, 0);
  block.writeUInt32LE(VERSION, 4);
  return block;
}

function putString(buffer, at, size, text) {
  const bytes = Buffer.from(String(text || ''), 'utf8');
  /* Cut on a character boundary and always leave a NUL. */
  let length = Math.min(bytes.length, size - 1);
  while (length > 0 && (bytes[length] & 0xc0) === 0x80) length--;
  bytes.copy(buffer, at, 0, length);
}

function encodeApp(row) {
  const out = Buffer.alloc(APP_BYTES);
  putString(out, 0, 64, row.id);
  putString(out, 64, 48, row.name);
  putString(out, 112, 16, row.version);
  putString(out, 128, 16, row.installed || '');
  putString(out, 144, 32, row.author);
  putString(out, 176, 16, row.category);
  putString(out, 192, 160, row.description);
  out.writeUInt32LE(Math.min(Number(row.size) || 0, 0xffffffff), 352);
  out.writeUInt32LE((row.installed ? 1 : 0) | (row.update ? 2 : 0) |
    (row.kind === 'gpubrew' ? 4 : 0), 356);
  return out;
}

class Services {
  constructor({appsDir, log, validate, catalogUrl}) {
    this.appsDir = appsDir;
    this.log = log;
    this.validate = validate;
    this.catalogUrl = catalogUrl || pkgmgr.DEFAULT_CATALOG;
    this.index = null;
    this.changed = false;
  }

  static declared(manifest) {
    const list = manifest.services || [];
    return Array.isArray(list) ? list.filter(name => SERVICES.has(name)) : [];
  }

  async catalog(refresh) {
    if (!this.index || refresh)
      this.index = await pkgmgr.loadCatalog({url: this.catalogUrl, refresh});
    return this.index;
  }

  /* Mutates `block` in place with the reply. */
  async handle(block, allowed) {
    const op = block.readUInt32LE(8);
    const sequence = block.readUInt32LE(12);
    const requestBytes = Math.min(block.readUInt32LE(20), DATA_BYTES);
    const a0 = block.readUInt32LE(24);
    const text = block.subarray(DATA, DATA + requestBytes).toString('utf8').replace(/\0.*$/s, '').trim();
    let result = 0, r0 = 0, r1 = 0, reply = Buffer.alloc(0);

    const fail = (code, message) => {
      result = ERR[code] || ERR.io;
      reply = Buffer.from(message + '\0', 'utf8');
    };
    try {
      if (!NEEDS[op]) fail('unknown', `unknown service op ${op}`);
      else if (!allowed.includes(NEEDS[op])) fail('denied', `service "${NEEDS[op]}" not declared`);
      else if (op === OP.INFO) {
        reply = Buffer.from([`catalog=${this.catalogUrl}`, `apps=${this.appsDir}`,
          `host=${process.platform}`, `node=${process.version}`].join('\n') + '\n\0');
      } else if (op === OP.CATALOG || op === OP.REFRESH) {
        const index = await this.catalog(op === OP.REFRESH);
        const rows = pkgmgr.status(index, this.appsDir)
          .sort((a, b) => String(a.name).localeCompare(String(b.name)));
        const perPage = Math.floor(DATA_BYTES / APP_BYTES);
        const page = rows.slice(a0, a0 + perPage);
        reply = Buffer.concat(page.map(encodeApp));
        r0 = rows.length;
        r1 = page.length;
      } else if (op === OP.INSTALL) {
        const index = await this.catalog(false);
        const app = index.apps.find(item => item.id === text);
        if (!app) fail('unknown', `${text} is not in the catalog`);
        else {
          const done = await pkgmgr.install(this.appsDir, app,
            {validate: this.validate, log: message => this.log.info(message)});
          this.changed = true;
          reply = Buffer.from(`${done.updated ? 'updated' : 'installed'} ${app.name} ${app.version}\0`);
        }
      } else if (op === OP.REMOVE) {
        pkgmgr.remove(this.appsDir, text);
        this.changed = true;
        reply = Buffer.from(`removed ${text}\0`);
      }
    } catch (error) {
      this.log.warn('service call failed', {op, error: error.message});
      fail(error.code, error.message);
    }

    reply = reply.subarray(0, DATA_BYTES);
    block.fill(0, DATA, DATA + DATA_BYTES);
    reply.copy(block, DATA);
    block.writeUInt32LE(0, 8);
    block.writeInt32LE(result, 16);
    block.writeUInt32LE(reply.length, 20);
    block.writeUInt32LE(r0, 32);
    block.writeUInt32LE(r1, 36);
    block.writeUInt32LE(sequence, 40);
    return {op, result};
  }
}

module.exports = {Services, freshBlock, encodeApp, CALL, BLOCK_BYTES, OP, MAGIC, SCRATCH, DATA};
