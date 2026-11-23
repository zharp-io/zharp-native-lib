// What the stand-in received, checked: each crash walked with rust-minidump's minidump-stackwalk
// (the walker Zharp's backend runs) and the .sym files `zharp debug-files upload` sent, laid out
// the way the backend lays them out; the handled errors' frames named from the same .sym files.
//
//   node check.mjs <out dir> <modes…>     (modes in the order the crashes were sent)
import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, readdirSync, writeFileSync, copyFileSync } from 'node:fs';
import { join } from 'node:path';

const out = process.argv[2];
const modes = process.argv.slice(3);
let failures = 0;
const check = (ok, what) => {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}`);
  if (!ok) failures++;
};

// ── The .sym files the CLI uploaded, where the walker looks for them ─────────
const symbols = join(out, 'symbols');
const syms = new Map(); // debug ID → .sym text
for (const name of readdirSync(join(out, 'artifacts')).filter((n) => n.endsWith('.json'))) {
  const meta = JSON.parse(readFileSync(join(out, 'artifacts', name), 'utf8'));
  if (meta.kind !== 'breakpad-sym') continue;
  const file = join(out, 'artifacts', name.replace(/\.json$/, '.breakpad-sym'));
  const text = readFileSync(file, 'utf8');
  const [, , , id, debugFile] = text.split('\n', 1)[0].trim().split(' ');
  syms.set(id, text);
  // minidump-walker.ts symbolPath(): <debug file>/<DEBUG ID>/<debug file without .pdb>.sym
  const dir = join(symbols, debugFile, id);
  mkdirSync(dir, { recursive: true });
  copyFileSync(file, join(dir, `${debugFile.replace(/\.pdb$/i, '')}.sym`));
  console.log(`symbols: ${debugFile} ${id} (uploaded by the CLI as ${meta.name})`);
}
check(syms.size > 0, '.sym files uploaded');

// ── Crashes ─────────────────────────────────────────────────────────────────
// Names as each compiler's debug info writes them ("run(char const*)" from DWARF, "run(const char *)"
// from a PDB); crash reasons as each OS has them.
const ABORT = /^(SIGABRT|EXC_CRASH|EXC_SOFTWARE|STATUS_FATAL_APP_EXIT|0x40000015)/;
// With MSVC an uncaught exception is SEH exception 0xE06D7363 and Crashpad catches that, not the abort.
const UNCAUGHT = /^(SIGABRT|EXC_CRASH|EXC_SOFTWARE|STATUS_FATAL_APP_EXIT|0x40000015|Unhandled C\+\+ Exception)/;
// "main" from DWARF, "main(int, char**)" from a PDB.
const MAIN = /^main(\(|$)/;
const expected = {
  segv: { reason: /^(SIGSEGV|SIGBUS|EXC_BAD_ACCESS|EXCEPTION_ACCESS_VIOLATION)/, frames: [/^Cart::total\(/, /^Shop::checkout\(/, /^run\(/, MAIN], file: 'cart.cpp' },
  abort: { reason: ABORT, abort: true, frames: [/^validate\(/, /^run\(/, MAIN], file: 'main.cpp' },
  throw: { reason: UNCAUGHT, abort: true, frames: [/^load_inventory\(/, /^run\(/, MAIN], file: 'main.cpp', exception: ['std::runtime_error', 'inventory file is corrupt'] },
};

const dumps = readdirSync(join(out, 'minidumps')).filter((n) => n.endsWith('.dmp')).sort((a, b) => parseInt(a) - parseInt(b));
check(dumps.length === modes.length, `${modes.length} crash reports arrived (got ${dumps.length})`);
dumps.forEach((name, i) => {
  const mode = modes[i];
  const want = expected[mode];
  const { key, annotations } = JSON.parse(readFileSync(join(out, 'minidumps', name.replace('.dmp', '.json')), 'utf8'));
  const json = execFileSync('minidump-stackwalk', ['--json', '--symbols-path', symbols, join(out, 'minidumps', name)], { maxBuffer: 256 << 20, stdio: ['ignore', 'pipe', 'ignore'] }).toString();
  writeFileSync(join(out, 'minidumps', name.replace('.dmp', '.walk.json')), json);
  const walk = JSON.parse(json);
  const thread = walk.crashing_thread ?? walk.threads[walk.crash_info.crashing_thread];
  console.log(`\n${mode}: ${walk.crash_info.type} at ${walk.crash_info.address} (${walk.system_info.os} ${walk.system_info.cpu_arch})`);
  const lines = [];
  for (const frame of thread.frames.slice(0, 14)) {
    for (const call of [...(frame.inlines ?? []), frame]) {
      const where = call.file ? ` (${call.file}:${call.line})` : '';
      lines.push(`#${lines.length}`.padEnd(4) + (call.function ? `${call.function}${where} [${frame.module} +${frame.module_offset}]` : `${frame.module ?? '???'} +${frame.module_offset ?? frame.offset}`));
    }
  }
  console.log(lines.map((l) => `    ${l}`).join('\n'));
  check(key === 'zpk_e2e', 'the key came in the URL');
  // macOS: an abort is a signal with no Mach exception behind it, so the EXC_CRASH Crashpad catches
  // unwraps to exception 0 (ExcCrashRecoverOriginalException drops the signal number). Such a dump
  // is an abort when its crashing thread is in abort().
  const macAbort = want.abort && /^Mac/.test(walk.system_info.os) && /^unknown 0x00000000 /.test(walk.crash_info.type) &&
    thread.frames.slice(0, 4).some((f) => f.module === 'libsystem_c.dylib');
  check(want.reason.test(walk.crash_info.type) || macAbort, `crash reason ${walk.crash_info.type}${macAbort ? ' (an abort on macOS)' : ''}`);
  const named = thread.frames.flatMap((f) => [...(f.inlines ?? []).map((x) => x.function), f.function]).filter(Boolean);
  // Linux ARM64, uncaught exception: libstdc++'s __verbose_terminate_handler calls abort() without a
  // frame of its own, so the return into the terminate handler is only in the frame-pointer chain,
  // which doesn't give the handler's stack pointer; the walk ends there. A stack recorded in-process
  // where the crash is named is the way around it.
  const walkEnds = mode === 'throw' && walk.system_info.os === 'Linux' && walk.system_info.cpu_arch === 'arm64' &&
    named.includes('zharp_native_terminate_handler()');
  if (walkEnds) {
    console.log('  skip the frames below the terminate handler (the walk ends in it on Linux ARM64)');
  } else {
    for (const fn of want.frames) check(named.some((name) => fn.test(name)), `frame ${fn.source}`);
    check(thread.frames.some((f) => (f.file ?? '').endsWith(want.file) && f.line > 0), `a frame in ${want.file} with its line`);
  }
  check(annotations['zharp.release'] === '1.0.0', 'release 1.0.0');
  check(annotations['zharp.environment'] === 'e2e', 'environment e2e');
  check(annotations['zharp.service'] === 'shop', 'service shop');
  check(/^[0-9a-f-]{36}$/.test(annotations['zharp.session_id'] ?? ''), 'a session ID');
  check(/^[0-9a-f-]{36}$/.test(annotations['zharp.device_id'] ?? ''), 'a device ID');
  check(annotations['zharp.user.id'] === 'u_42' && annotations['zharp.user.email'] === 'ada@example.com', 'the user');
  check(annotations.store === 'berlin', 'the tag store=berlin');
  check((annotations['zharp.breadcrumbs'] ?? '').includes('info shop started') && annotations['zharp.breadcrumbs'].includes('ui opened the cart'), 'breadcrumbs');
  if (want.exception) {
    check(annotations['zharp.exception.type'] === want.exception[0], `uncaught exception type ${annotations['zharp.exception.type']}`);
    check(annotations['zharp.exception.message'] === want.exception[1], `uncaught exception message "${annotations['zharp.exception.message']}"`);
  }
});

// ── Handled errors: frames named from the same .sym files ───────────────────
/**
 * The frames at an offset in a Breakpad .sym, innermost first: inlined calls (INLINE records),
 * then the function (FUNC) they're in; a PUBLIC name when no FUNC covers it. The same reading
 * as the backend's breakpad-sym.ts.
 */
function lookup(text, offset) {
  const files = new Map();
  const origins = new Map();
  let hit = null;
  let inside = false;
  let publicBest = null;
  for (const line of text.split('\n')) {
    const parts = line.trim().split(' ');
    if (parts[0] === 'FILE') files.set(parts[1], parts.slice(2).join(' '));
    else if (parts[0] === 'INLINE_ORIGIN') origins.set(parts[1], parts.slice(2).join(' '));
    else if (parts[0] === 'FUNC') {
      const m = parts[1] === 'm' ? 1 : 0;
      const address = parseInt(parts[1 + m], 16);
      inside = offset >= address && offset < address + parseInt(parts[2 + m], 16);
      if (inside) hit = { name: parts.slice(4 + m).join(' '), file: '', line: null, inlines: [] };
    } else if (parts[0] === 'PUBLIC') {
      inside = false;
      const m = parts[1] === 'm' ? 1 : 0;
      const address = parseInt(parts[1 + m], 16);
      if (address <= offset && (!publicBest || address > publicBest.address)) publicBest = { address, name: parts.slice(3 + m).join(' ') };
    } else if (inside && parts[0] === 'INLINE') {
      for (let i = 5; i + 1 < parts.length; i += 2) {
        const a = parseInt(parts[i], 16);
        if (offset >= a && offset < a + parseInt(parts[i + 1], 16)) hit.inlines.push({ depth: +parts[1], line: +parts[2], file: parts[3], origin: parts[4] });
      }
    } else if (inside && /^[0-9a-f]+ [0-9a-f]+ \d+ \d+$/.test(line.trim())) {
      const a = parseInt(parts[0], 16);
      if (offset >= a && offset < a + parseInt(parts[1], 16)) Object.assign(hit, { line: +parts[2], file: files.get(parts[3]) });
    }
  }
  if (!hit) return publicBest ? [{ name: publicBest.name }] : [];
  const chain = hit.inlines.sort((a, b) => a.depth - b.depth);
  const frames = [];
  let where = hit.line ? `${hit.file}:${hit.line}` : '';
  for (let i = chain.length - 1; i >= 0; i--) {
    frames.push({ name: origins.get(chain[i].origin), where });
    where = `${files.get(chain[i].file)}:${chain[i].line}`;
  }
  frames.push({ name: hit.name, where });
  return frames;
}

const records = [];
for (const name of readdirSync(join(out, 'otlp')).filter((n) => n.endsWith('-logs.json'))) {
  const body = JSON.parse(readFileSync(join(out, 'otlp', name), 'utf8'));
  for (const rl of body.resourceLogs) for (const sl of rl.scopeLogs) for (const r of sl.logRecords) records.push({ r, resource: rl.resource });
}
const attr = (list, key) => list.find((a) => a.key === key)?.value?.stringValue;
const errors = records.filter(({ r }) => attr(r.attributes, 'exception.type'));
console.log(`\nhandled errors: ${errors.map(({ r }) => attr(r.attributes, 'exception.type')).join(', ')}`);
// C++ keeps no stack of a throw: a caught exception's stack is where it was reported, in run()'s catch
// (with MSVC, the catch block's own funclet: "`run'::`1'::catch$8").
const wanted = {
  PaymentDeclined: [/ (run\(|`run'::)/, 'main.cpp:'],
  InventoryError: [/ run\(/, 'main.cpp:'],
  Message: [/ run\(/, 'main.cpp:'],
};
for (const [type, [fn, file]] of Object.entries(wanted)) {
  const found = errors.find(({ r }) => attr(r.attributes, 'exception.type') === type);
  check(!!found, `handled error ${type} sent`);
  if (!found) continue;
  const modules = new Map((attr(found.r.attributes, 'zharp.native_modules') ?? '').split('\n').map((l) => l.split('\t')).map(([n, id]) => [n, id]));
  const stack = attr(found.r.attributes, 'exception.stacktrace');
  const readable = [];
  for (const line of stack.split('\n')) {
    const m = /^#(\d+)\s+(?:(.+?) \[)?(.+?) \+(0x[0-9a-f]+)\]?$/.exec(line);
    const id = m && modules.get(m[3]);
    const frames = id && syms.has(id) ? lookup(syms.get(id), parseInt(m[4], 16) - 1) : [];
    if (!frames.length) {
      readable.push(m ? `#${readable.length - 1}`.padEnd(4) + line.replace(/^#\d+\s+/, '') : line);
      continue;
    }
    for (const frame of frames) readable.push(`#${readable.length - 1}`.padEnd(4) + `${frame.name}${frame.where ? ` (${frame.where})` : ''} [${m[3]} +${m[4]}]`);
  }
  console.log(`  ${type}:\n${readable.slice(0, 8).map((l) => `      ${l}`).join('\n')}`);
  const culprit = readable.slice(1).find((l) => !/\bzharp(::|_)/.test(l));
  check(!!culprit && fn.test(culprit) && culprit.includes(file), `${type}: first frame outside Zharp is run() in ${file.replace(/:$/, '')} (named from the .sym)`);
  check(attr(found.r.attributes, 'zharp.handled') === 'true', `${type} marked handled`);
  check(attr(found.resource.attributes, 'service.version') === '1.0.0', `${type} carries the release`);
  check((attr(found.r.attributes, 'zharp.breadcrumbs') ?? '').includes('info shop started'), `${type} carries the breadcrumbs`);
}
const logged = records.find(({ r }) => r.body?.stringValue?.startsWith('report written: 120 rows'));
check(!!logged, 'ZHARP_LOGS=true sent the log line');
check(!!logged && / trace_id=[0-9a-f]{32} span_id=[0-9a-f]{16}$/.test(logged.r.body.stringValue), 'the line carries its trace stamp');
const spans = readdirSync(join(out, 'otlp')).filter((n) => n.endsWith('-traces.json')).map((n) => readFileSync(join(out, 'otlp', n), 'utf8')).join('\n');
check(spans.includes('"name":"nightly report"') && spans.includes('"name":"checkout"'), 'spans sent');

console.log(failures ? `\n${failures} check(s) failed` : '\nall checks passed');
process.exit(failures ? 1 : 0);
