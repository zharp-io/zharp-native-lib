// A stand-in for the Zharp API, keeping what each request sent:
//   POST /api/v1/minidump?key=…                     Crashpad's upload (multipart, gzipped)
//   POST /api/v1/otlp/v1/logs | /v1/traces           the SDK's OTLP/HTTP JSON
//   POST /api/v1/applications/:id/artifacts          `zharp debug-files upload` (.sym, source bundles)
//
//   node receiver.mjs <out dir> <port>
import { createServer } from 'node:http';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { gunzipSync } from 'node:zlib';

const out = process.argv[2] || '/out';
const port = Number(process.argv[3] || 8099);
for (const dir of ['minidumps', 'otlp', 'artifacts']) mkdirSync(join(out, dir), { recursive: true });

/** Fields and files of a multipart/form-data body. */
export function parseMultipart(body, type) {
  const boundary = /boundary=(?:"([^"]+)"|([^;\s]+))/i.exec(type || '');
  if (!boundary) return [];
  const marker = Buffer.from(`--${boundary[1] ?? boundary[2]}`);
  const parts = [];
  let at = body.indexOf(marker);
  while (at >= 0) {
    let start = at + marker.length;
    if (body[start] === 0x2d && body[start + 1] === 0x2d) break;
    if (body[start] === 0x0d) start += 2;
    const next = body.indexOf(marker, start);
    if (next < 0) break;
    const part = body.subarray(start, next - 2);
    const split = part.indexOf('\r\n\r\n');
    const headers = part.subarray(0, split).toString();
    parts.push({
      name: /name="([^"]*)"/.exec(headers)?.[1] ?? '',
      filename: /filename="([^"]*)"/.exec(headers)?.[1] ?? null,
      data: part.subarray(split + 4),
    });
    at = next;
  }
  return parts;
}

let dumps = 0;
let otlp = 0;
let artifacts = 0;

createServer((req, res) => {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    const raw = Buffer.concat(chunks);
    const url = new URL(req.url, 'http://localhost');
    try {
      if (req.method === 'POST' && url.pathname === '/api/v1/minidump') {
        const n = ++dumps;
        // The request as Crashpad sent it, for the backend's tests.
        writeFileSync(join(out, 'minidumps', `${n}.request`), raw);
        writeFileSync(join(out, 'minidumps', `${n}.headers.json`), JSON.stringify(req.headers, null, 2));
        const body = req.headers['content-encoding'] === 'gzip' ? gunzipSync(raw) : raw;
        const parts = parseMultipart(body, req.headers['content-type']);
        const annotations = {};
        for (const part of parts) {
          if (part.name === 'upload_file_minidump') writeFileSync(join(out, 'minidumps', `${n}.dmp`), part.data);
          else if (part.filename === null) annotations[part.name] = part.data.toString();
        }
        writeFileSync(join(out, 'minidumps', `${n}.json`), JSON.stringify({ key: url.searchParams.get('key'), annotations }, null, 2));
        console.log(`[receiver] minidump ${n}: ${parts.find((p) => p.name === 'upload_file_minidump')?.data.length ?? 0} bytes, gzip=${req.headers['content-encoding'] === 'gzip'}, key=${url.searchParams.get('key')}`);
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.end(`crash-${n}`);
        return;
      }
      if (req.method === 'POST' && url.pathname.startsWith('/api/v1/otlp/v1/')) {
        const n = ++otlp;
        const signal = url.pathname.endsWith('/logs') ? 'logs' : 'traces';
        writeFileSync(join(out, 'otlp', `${String(n).padStart(3, '0')}-${signal}.json`), raw);
        writeFileSync(join(out, 'otlp', `${String(n).padStart(3, '0')}-${signal}.auth`), String(req.headers.authorization || ''));
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end('{}');
        return;
      }
      const artifact = /^\/api\/v1\/applications\/([^/]+)\/artifacts$/.exec(url.pathname);
      if (req.method === 'POST' && artifact) {
        const n = ++artifacts;
        const parts = parseMultipart(raw, req.headers['content-type']);
        const fields = Object.fromEntries(parts.filter((p) => p.filename === null).map((p) => [p.name, p.data.toString()]));
        const file = parts.find((p) => p.name === 'file');
        const data = file.filename.endsWith('.gz') ? gunzipSync(file.data) : file.data;
        writeFileSync(join(out, 'artifacts', `${n}.${fields.kind}`), data);
        writeFileSync(join(out, 'artifacts', `${n}.json`), JSON.stringify({ app: artifact[1], ...fields, filename: file.filename, bytes: data.length }, null, 2));
        console.log(`[receiver] artifact ${n}: ${fields.kind} ${fields.name} ${fields.debugId}`);
        res.writeHead(201, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ id: n, kind: fields.kind, debugId: fields.debugId, name: fields.name }));
        return;
      }
      res.writeHead(404);
      res.end('not found');
    } catch (error) {
      console.error('[receiver]', error);
      res.writeHead(500);
      res.end(String(error));
    }
  });
}).listen(port, '127.0.0.1', () => console.log(`[receiver] listening on ${port}, writing to ${out}`));
