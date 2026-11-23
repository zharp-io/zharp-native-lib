// What the stand-in received from the Qt shops, checked: each crash walked with rust-minidump's
// minidump-stackwalk and the .sym files the CLI uploaded (as Zharp's backend walks them), with
// the screen, the breadcrumbs and qFatal's reason; the warning sent as a log; the HTTP spans and
// the traceparent the prices service got.
//
//   node check.mjs <out dir> <shop:mode…>     (in the order the crashes were sent)
import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const out = process.argv[2];
const runs = process.argv.slice(3).map((r) => r.split(':'));
let failures = 0;
const check = (ok, what) => {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}`);
  if (!ok) failures++;
};

// ── The uploaded .sym files, laid out as the backend's walker reads them ────
const symbols = join(out, 'symbols');
const syms = new Map(); // debug ID → .sym text
for (const name of readdirSync(join(out, 'artifacts')).filter((n) => n.endsWith('.json'))) {
  const meta = JSON.parse(readFileSync(join(out, 'artifacts', name), 'utf8'));
  if (meta.kind !== 'breakpad-sym') continue;
  const file = join(out, 'artifacts', name.replace(/\.json$/, '.breakpad-sym'));
  const [, , , id, debugFile] = readFileSync(file, 'utf8').split('\n', 1)[0].trim().split(' ');
  const dir = join(symbols, debugFile, id);
  mkdirSync(dir, { recursive: true });
  copyFileSync(file, join(dir, `${debugFile}.sym`));
  syms.set(id, readFileSync(file, 'utf8'));
  console.log(`symbols: ${debugFile} ${id}`);
}

// Roughly the backend's NOT_IN_APP for native modules (src/ingest/errors/fingerprint.ts): the
// system's libraries, Qt's (and its plugins) and Zharp's.
const SYSTEM = new RegExp('^(' + [
  'libc', 'libm', 'libdl', 'librt', 'libpthread', 'libgcc_s', 'libstdc\\+\\+', 'libc\\+\\+(abi)?', 'ld-linux[\\w.-]*', 'linux-vdso',
  'ntdll', 'kernel32', 'kernelbase', 'ucrtbased?', 'vcruntime\\d*d?(_\\d+)?', 'msvcp\\d*d?(_\\w+)?', 'user32', 'win32u',
  'libsystem_\\w+', 'libdyld', 'dyld', 'libobjc[\\w.]*', 'CoreFoundation', 'Foundation', 'AppKit', 'libdispatch',
  '(lib)?Qt[56]\\w*', 'Qt(Core|Gui|Widgets|Network|Qml\\w*|Quick\\w*|OpenGL\\w*|DBus)', 'libq\\w+', 'q(windows|offscreen|minimal|cocoa)\\w*',
  'libzharp\\w*', 'zharp(_qt)?',
].join('|') + ')([.-].*)?$', 'i');
const HANDLER = /^(crashpad::|google_breakpad::|zharp_native_|zharp::|ZharpQt::)/;

const ABORT = /^(SIGABRT|EXC_CRASH|EXC_SOFTWARE|STATUS_FATAL_APP_EXIT|0x40000015)/;
const SEGV = /^(SIGSEGV|SIGBUS|EXC_BAD_ACCESS|EXCEPTION_ACCESS_VIOLATION)/;
const expected = {
  'qt_shop:crash': { reason: SEGV, top: /^Basket::total\(/, frames: [/^CheckoutDialog::confirm\(/], file: 'shop_window.cpp', screen: 'CheckoutDialog', way: 'ShopWindow → CheckoutDialog' },
  'qt_shop:fatal': { reason: ABORT, top: /^CheckoutDialog::loadPaymentMethods\(/, frames: [], file: 'shop_window.cpp', screen: 'CheckoutDialog', fatal: 'payment methods table is missing (schema 7)' },
  'qt_qml:crash': { reason: SEGV, top: /^Gateway::send\(/, frames: [/^Payments::charge\(/], file: 'payments.cpp', screen: 'PaymentPage', way: 'CartPage → PaymentPage' },
  'qt_qml:fatal': { reason: ABORT, top: /^Payments::openDatabase\(/, frames: [], file: 'payments.cpp', screen: 'PaymentPage', fatal: 'payments.db is missing' },
};

const dumps = readdirSync(join(out, 'minidumps')).filter((n) => n.endsWith('.dmp')).sort((a, b) => parseInt(a) - parseInt(b));
check(dumps.length === runs.length, `${runs.length} crash reports arrived (got ${dumps.length})`);
dumps.forEach((name, i) => {
  if (!runs[i]) return;
  const [shop, mode] = runs[i];
  const want = expected[`${shop}:${mode}`];
  const { key, annotations } = JSON.parse(readFileSync(join(out, 'minidumps', name.replace('.dmp', '.json')), 'utf8'));
  const json = execFileSync('minidump-stackwalk', ['--json', '--symbols-path', symbols, join(out, 'minidumps', name)], { maxBuffer: 256 << 20, stdio: ['ignore', 'pipe', 'ignore'] }).toString();
  writeFileSync(join(out, 'minidumps', name.replace('.dmp', '.walk.json')), json);
  const walk = JSON.parse(json);
  const thread = walk.crashing_thread ?? walk.threads[walk.crash_info.crashing_thread];
  const calls = thread.frames.flatMap((f) => [...(f.inlines ?? []).map((x) => ({ ...x, module: f.module, inline: true })), f]);
  console.log(`\n${shop} ${mode} (${annotations['zharp.sdk']}): ${walk.crash_info.type}${annotations['zharp.exception.type'] ? ` → ${annotations['zharp.exception.type']}: ${annotations['zharp.exception.message']}` : ''}`);
  calls.slice(0, 16).forEach((c, n) => {
    const where = c.file ? ` (${c.file.split('/').pop()}:${c.line})` : '';
    const inApp = c.module && !SYSTEM.test(c.module) && !HANDLER.test(c.function ?? '') ? '*' : ' ';
    console.log(`   ${inApp}#${String(n).padEnd(3)}${c.function ? `${c.function}${where}` : `+${c.module_offset}`} [${c.module ?? '???'}]`);
  });
  check(key === 'zpk_e2e', 'the key came in the URL');
  check(want.reason.test(walk.crash_info.type), `crash reason ${walk.crash_info.type}`);
  // A crash the SDK named (qFatal) carries the stack recorded where it was named: Zharp reads that,
  // because the walk can't unwind abort() through libraries it has no .sym for.
  const recorded = annotations['zharp.exception.stacktrace'] ? named(annotations['zharp.exception.stacktrace'], annotations['zharp.exception.modules']) : null;
  if (recorded) {
    console.log('   recorded where it was named:');
    recorded.slice(0, 12).forEach((c, n) => console.log(`   ${!SYSTEM.test(c.module) && !HANDLER.test(c.function ?? '') ? '*' : ' '}#${String(n).padEnd(3)}${c.function ?? `+${c.module_offset}`}${c.file ? ` (${c.file.split(/[\\/]/).pop()}:${c.line})` : ''} [${c.module}]`));
  }
  const frames = recorded ?? calls;
  // The first frame in the app (not Qt, the C library or Zharp) is where it went wrong.
  const culprit = frames.find((c) => c.function && c.module && !SYSTEM.test(c.module) && !HANDLER.test(c.function));
  check(!!culprit && want.top.test(culprit.function), `first frame in the app: ${culprit?.function}`);
  for (const fn of want.frames) check(frames.some((c) => fn.test(c.function ?? '')), `frame ${fn.source}`);
  check(frames.some((c) => (c.file ?? '').endsWith(want.file) && c.line > 0), `a frame in ${want.file} with its line`);
  if (want.fatal) check(!!recorded, 'the stack where qFatal was called is in the report');
  check(calls.some((c) => /^(lib)?Qt[56]?[A-Z]\w*/.test(c.module ?? '') && SYSTEM.test(c.module)), "Qt's own frames are there (and not the app's)");
  check(annotations['zharp.service'] === shop, `service ${shop}`);
  check(annotations['zharp.release'] === '2.0.0', 'release 2.0.0');
  check(/^[0-9a-f-]{36}$/.test(annotations['zharp.session_id'] ?? ''), 'a session ID');
  check(annotations['zharp.user.id'] === 'u_42' || shop === 'qt_qml', 'the user');
  check(annotations['screen.name'] === want.screen, `screen.name ${annotations['screen.name']}`);
  const crumbs = annotations['zharp.breadcrumbs'] ?? '';
  check(crumbs.includes('payment provider is slow (2300 ms)'), 'the warning is a breadcrumb');
  if (want.way) check(crumbs.includes(`navigation ${want.way}`), `the way there: ${want.way}`);
  if (want.fatal) {
    check(annotations['zharp.exception.type'] === 'qFatal', 'qFatal names the crash');
    check(annotations['zharp.exception.message'] === want.fatal, `qFatal's message "${annotations['zharp.exception.message']}"`);
  } else {
    check(!annotations['zharp.exception.type'], 'no crash reason recorded for a plain crash');
  }
});

/** A recorded "#0  module +0xoff" stack, named from the .sym files (FUNC, line and INLINE records) as the backend's breakpad-sym.ts reads them. */
function named(stack, modulesText) {
  const ids = new Map((modulesText ?? '').split('\n').map((l) => l.split('\t')).map(([n, id]) => [n, id]));
  const out = [];
  for (const line of stack.split('\n')) {
    const m = /^#(\d+)\s+(?:(.+?) \[)?(.+?) \+(0x[0-9a-f]+)\]?$/.exec(line);
    if (!m) continue;
    const text = syms.get(ids.get(m[3]));
    const found = text ? lookup(text, parseInt(m[4], 16) - 1) : [];
    if (!found.length) out.push({ function: m[2], module: m[3], module_offset: m[4] });
    for (const f of found) out.push({ function: f.name, file: f.file, line: f.line, module: m[3] });
  }
  return out;
}

function lookup(text, offset) {
  const files = new Map();
  const origins = new Map();
  let hit = null;
  let inside = false;
  for (const line of text.split('\n')) {
    const parts = line.trim().split(' ');
    if (parts[0] === 'FILE') files.set(parts[1], parts.slice(2).join(' '));
    else if (parts[0] === 'INLINE_ORIGIN') origins.set(parts[1], parts.slice(2).join(' '));
    else if (parts[0] === 'FUNC') {
      const k = parts[1] === 'm' ? 1 : 0;
      const address = parseInt(parts[1 + k], 16);
      inside = offset >= address && offset < address + parseInt(parts[2 + k], 16);
      if (inside) hit = { name: parts.slice(4 + k).join(' '), file: '', line: null, inlines: [] };
    } else if (parts[0] === 'PUBLIC') inside = false;
    else if (inside && parts[0] === 'INLINE') {
      for (let i = 5; i + 1 < parts.length; i += 2) {
        const a = parseInt(parts[i], 16);
        if (offset >= a && offset < a + parseInt(parts[i + 1], 16)) hit.inlines.push({ depth: +parts[1], line: +parts[2], file: parts[3], origin: parts[4] });
      }
    } else if (inside && /^[0-9a-f]+ [0-9a-f]+ \d+ \d+$/.test(line.trim())) {
      const a = parseInt(parts[0], 16);
      if (offset >= a && offset < a + parseInt(parts[1], 16)) Object.assign(hit, { line: +parts[2], file: files.get(parts[3]) });
    }
  }
  if (!hit) return [];
  const chain = hit.inlines.sort((a, b) => a.depth - b.depth);
  const frames = [];
  let file = hit.file;
  let ln = hit.line;
  for (let i = chain.length - 1; i >= 0; i--) {
    frames.push({ name: origins.get(chain[i].origin), file, line: ln });
    file = files.get(chain[i].file);
    ln = chain[i].line;
  }
  frames.push({ name: hit.name, file, line: ln });
  return frames;
}

// ── The runs that didn't crash: the warning as a log, the HTTP spans ────────
const read = (signal) =>
  readdirSync(join(out, 'otlp'))
    .filter((n) => n.endsWith(`-${signal}.json`))
    .map((n) => JSON.parse(readFileSync(join(out, 'otlp', n), 'utf8')));
const attr = (list, key) => list?.find((a) => a.key === key)?.value?.stringValue;
const logs = read('logs').flatMap((b) => b.resourceLogs.flatMap((rl) => rl.scopeLogs.flatMap((sl) => sl.logRecords.map((r) => ({ r, service: attr(rl.resource.attributes, 'service.name') })))));
const spans = read('traces').flatMap((b) => b.resourceSpans.flatMap((rs) => rs.scopeSpans.flatMap((ss) => ss.spans.map((s) => ({ s, service: attr(rs.resource.attributes, 'service.name') })))));
console.log('');
for (const shop of [...new Set(runs.map(([shop]) => shop))]) {
  const warning = logs.find(({ r, service }) => service === shop && /payment provider is slow \(2300 ms\)$/.test(r.body?.stringValue ?? ''));
  check(!!warning && warning.r.severityText === 'WARN', `${shop}: the warning sent as a log (ZHARP_LOGS=true): "${warning?.r.body.stringValue}"`);
  if (shop === 'qt_qml') check(attr(warning?.r.attributes, 'screen.name') === 'CartPage', `${shop}: logged on screen ${attr(warning?.r.attributes, 'screen.name')}`);
  if (shop === 'qt_shop') check(attr(warning?.r.attributes, 'screen.name') === 'CheckoutDialog', `${shop}: logged on screen ${attr(warning?.r.attributes, 'screen.name')}`);
  const call = spans.find(({ s, service }) => service === shop && s.name === 'GET 127.0.0.1');
  check(!!call, `${shop}: the prices lookup is a span "GET 127.0.0.1"`);
  if (!call) continue;
  const a = call.s.attributes;
  check(call.s.kind === 3, `${shop}: a client span`);
  check(/\/shop\/prices$/.test(attr(a, 'url.full') ?? ''), `${shop}: url.full without its query (${attr(a, 'url.full')})`);
  check(attr(a, 'http.response.status_code') === '200', `${shop}: status 200`);
  check(attr(a, 'http.request.header.authorization') === '[REDACTED]', `${shop}: Authorization [REDACTED]`);
  check(attr(a, 'http.response.header.set-cookie') === '[REDACTED]', `${shop}: Set-Cookie [REDACTED]`);
  check(attr(a, 'http.response.header.x-shop-region') === 'eu', `${shop}: response headers`);
  check(!JSON.stringify(a).includes('s3cret') && !JSON.stringify(a).includes('shop-secret') && !JSON.stringify(a).includes('abc123'), `${shop}: no secrets on the span`);
  const sent = readdirSync(join(out, 'http')).map((n) => JSON.parse(readFileSync(join(out, 'http', n), 'utf8')));
  check(sent.some((h) => h.headers.traceparent === `00-${call.s.traceId}-${call.s.spanId}-01`), `${shop}: the server got traceparent 00-${call.s.traceId}-${call.s.spanId}-01`);
}

console.log(failures ? `\n${failures} check(s) failed` : '\nall checks passed');
process.exit(failures ? 1 : 0);
