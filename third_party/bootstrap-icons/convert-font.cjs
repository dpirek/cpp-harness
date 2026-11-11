// Convert the pinned upstream WOFF font to a Windows-loadable TrueType font.
// Run with Node.js only when refreshing the vendored font, not for normal builds.
const fs = require('node:fs');
const zlib = require('node:zlib');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, 'bootstrap-icons.woff'));
if (source.toString('ascii', 0, 4) !== 'wOFF') throw Error('Expected WOFF 1 font');
const count = source.readUInt16BE(12);
const output = Buffer.alloc(source.readUInt32BE(16));
source.copy(output, 0, 4, 8);
output.writeUInt16BE(count, 4);
const power = 2 ** Math.floor(Math.log2(count));
output.writeUInt16BE(power * 16, 6);
output.writeUInt16BE(Math.log2(power), 8);
output.writeUInt16BE(count * 16 - power * 16, 10);
let offset = 12 + count * 16;
for (let i = 0; i < count; ++i) {
  const table = 44 + i * 20, entry = 12 + i * 16;
  const start = source.readUInt32BE(table + 4);
  const compressed = source.readUInt32BE(table + 8);
  const length = source.readUInt32BE(table + 12);
  const bytes = source.subarray(start, start + compressed);
  const data = compressed === length ? bytes : zlib.inflateSync(bytes);
  if (data.length !== length) throw Error('Invalid font table length');
  source.copy(output, entry, table, table + 4);
  output.writeUInt32BE(source.readUInt32BE(table + 16), entry + 4);
  output.writeUInt32BE(offset, entry + 8);
  output.writeUInt32BE(length, entry + 12);
  data.copy(output, offset);
  offset += (length + 3) & ~3;
}
if (offset !== output.length) throw Error('Invalid font size');
fs.writeFileSync(path.join(__dirname, 'bootstrap-icons.ttf'), output);
