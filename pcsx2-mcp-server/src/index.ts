#!/usr/bin/env node
/**
 * PCSX2 MCP server.
 *
 * Talks to the DebugServer patched into PCSX2 (pcsx2-plugin/) over TCP; the
 * connection is made lazily and re-established automatically.
 *
 * Environment:
 *   PCSX2_DEBUG_PORT   DebugServer port (default 21512)
 *   PS2RECOMP_ROOT     enables the ps2recomp_* tools
 */

import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';
import * as fs from 'node:fs';
import * as path from 'node:path';
import {
  DebugServerClient, Cpu, Stop, RunResult, Status, RegisterCategory, Instruction,
  BreakpointInfo, WatchpointInfo, ThreadInfo, ModuleInfo, Frame, Location,
} from './debug-server-client.js';

const dbg = new DebugServerClient('127.0.0.1', Number(process.env.PCSX2_DEBUG_PORT) || 21512);
const PS2RECOMP_ROOT = process.env.PS2RECOMP_ROOT;

// ==========================================================
//  Helpers
// ==========================================================

type Content = { type: 'text'; text: string } | { type: 'image'; data: string; mimeType: string };
type ToolResult = { content: Content[]; isError?: boolean };

const server = new McpServer(
  { name: 'pcsx2', version: '3.0.0' },
  {
    instructions: [
      'Debug PlayStation 2 games running in PCSX2.',
      'Address parameters accept numbers or PCSX2 expressions: bare numbers are HEX ("1000e0" = 0x1000e0),',
      'and registers, symbols and arithmetic work ("a0+10", "sp", "main", "[gp-0x7ff0]" dereferences).',
      'Stepping, register writes and run_to need the VM paused: use pcsx2_pause first.',
      'pcsx2_continue with wait=true (or pcsx2_wait) blocks until a breakpoint/watchpoint fires and reports which one.',
      'Most tools target the EE (main CPU); pass cpu="iop" for the I/O processor.',
      'Drive the game with pcsx2_input and look at it with pcsx2_screenshot.',
    ].join(' '),
  },
);

function text(s: string): ToolResult { return { content: [{ type: 'text', text: s }] }; }

function tool<S extends z.ZodRawShape>(
  name: string,
  description: string,
  shape: S,
  run: (args: z.infer<z.ZodObject<S>>) => Promise<string | ToolResult>,
): void {
  server.registerTool(name, { description, inputSchema: shape }, (async (args: z.infer<z.ZodObject<S>>) => {
    try {
      const r = await run(args);
      return typeof r === 'string' ? text(r) : r;
    } catch (e: any) {
      return { content: [{ type: 'text', text: `Error: ${e?.message ?? e}` }], isError: true };
    }
  }) as any);
}

const addr = () => z.union([z.string(), z.number()]).describe('Address: number, or expression string (hex by default, e.g. "1000e0", "a0+10", "main")');
const cpuArg = () => z.enum(['ee', 'iop']).default('ee').describe('Target CPU');
const hex32 = (n: number) => '0x' + (n >>> 0).toString(16).padStart(8, '0');

async function readMem(a: string | number, length: number, cpu: Cpu = 'ee'): Promise<{ addr: number; data: Buffer }> {
  const r = await dbg.call<{ address: string; hex: string }>('read_memory', { address: a, length, cpu }, 60000);
  return { addr: parseInt(r.address, 16), data: Buffer.from(r.hex, 'hex') };
}

async function writeMem(a: string | number, data: Buffer, cpu: Cpu = 'ee'): Promise<number> {
  const r = await dbg.call<{ address: string }>('write_memory', { address: a, data: data.toString('hex'), cpu });
  return parseInt(r.address, 16);
}

function hexDump(buf: Buffer, base: number): string {
  const lines: string[] = [];
  for (let i = 0; i < buf.length; i += 16) {
    const row = buf.subarray(i, i + 16);
    const hex = [...row].map(b => b.toString(16).padStart(2, '0'));
    while (hex.length < 16) hex.push('  ');
    const ascii = [...row].map(c => (c >= 0x20 && c < 0x7f ? String.fromCharCode(c) : '.')).join('');
    lines.push(`${hex32(base + i).slice(2)}  ${hex.slice(0, 8).join(' ')}  ${hex.slice(8).join(' ')}  |${ascii}|`);
  }
  return lines.join('\n');
}

type ValueType = 'u8' | 's8' | 'u16' | 's16' | 'u32' | 's32' | 'u64' | 'f32' | 'f64';
const VALUE_SIZE: Record<ValueType, number> = { u8: 1, s8: 1, u16: 2, s16: 2, u32: 4, s32: 4, u64: 8, f32: 4, f64: 8 };

function readValue(buf: Buffer, off: number, t: ValueType): number | bigint {
  switch (t) {
    case 'u8': return buf.readUInt8(off);
    case 's8': return buf.readInt8(off);
    case 'u16': return buf.readUInt16LE(off);
    case 's16': return buf.readInt16LE(off);
    case 'u32': return buf.readUInt32LE(off);
    case 's32': return buf.readInt32LE(off);
    case 'u64': return buf.readBigUInt64LE(off);
    case 'f32': return buf.readFloatLE(off);
    case 'f64': return buf.readDoubleLE(off);
  }
}

function encodeValue(value: string, t: ValueType): Buffer {
  const b = Buffer.alloc(VALUE_SIZE[t]);
  const int = () => { const v = Number(value); if (!Number.isInteger(v)) throw new Error(`"${value}" is not an integer`); return v; };
  switch (t) {
    case 'u8': b.writeUInt8(int() & 0xff); break;
    case 's8': b.writeInt8(int()); break;
    case 'u16': b.writeUInt16LE(int() & 0xffff); break;
    case 's16': b.writeInt16LE(int()); break;
    case 'u32': b.writeUInt32LE(int() >>> 0); break;
    case 's32': b.writeInt32LE(int()); break;
    case 'u64': b.writeBigUInt64LE(BigInt(value)); break;
    case 'f32': b.writeFloatLE(Number(value)); break;
    case 'f64': b.writeDoubleLE(Number(value)); break;
  }
  return b;
}

function formatValue(v: number | bigint, t: ValueType): string {
  if (typeof v === 'bigint') return '0x' + v.toString(16).padStart(16, '0');
  if (t === 'f32' || t === 'f64') return String(v);
  if (t.startsWith('s')) return String(v);
  return '0x' + v.toString(16).padStart(VALUE_SIZE[t] * 2, '0');
}

/** GPRs as 32-bit when the value is a sign-extended word, else 64-bit, plus the upper half if used. */
function formatGpr(value128: string): string {
  const upper = value128.slice(0, 16), lower = value128.slice(16);
  const word = lower.slice(8);
  const signExtended = lower.slice(0, 8) === (parseInt(word[0], 16) >= 8 ? 'ffffffff' : '00000000');
  return '0x' + (signExtended ? word : lower) + (/^0+$/.test(upper) ? '' : `  [127:64] 0x${upper}`);
}

function where(loc: Location): string {
  const fn = loc.function ? ` <${loc.function}${loc.function_offset ? `+0x${loc.function_offset.toString(16)}` : ''}>` : '';
  return `${loc.pc}${fn}  ${loc.disasm}`;
}

function formatStop(stop: Stop, cpu: Cpu = 'ee'): string {
  if (!stop.alive) return 'VM is not running.';
  if (!stop.paused) return 'Still running.';
  const lines = [`Stopped (${stop.reason})`];
  for (const h of stop.hits) {
    if (h.type === 'breakpoint') lines.push(`  hit ${h.cpu.toUpperCase()} breakpoint ${h.address}${h.new_hits > 1 ? ` ×${h.new_hits}` : ''}`);
    else lines.push(`  hit ${h.cpu.toUpperCase()} watchpoint ${h.address}-${h.end}`
      + (h.last_pc ? `: ${h.last_size}-byte access to ${h.last_access} by PC ${h.last_pc}` : ' (not yet executed: the access is by the instruction at the stop PC or in its delay slot)'));
  }
  const main = cpu === 'iop' ? stop.iop : stop.ee;
  const other = cpu === 'iop' ? stop.ee : stop.iop;
  if (main) lines.push(`  ${cpu.toUpperCase()} ${where(main)}`);
  if (other) lines.push(`  ${cpu === 'iop' ? 'EE' : 'IOP'} ${other.pc}${other.function ? ` <${other.function}>` : ''}`);
  return lines.join('\n');
}

// ==========================================================
//  Status
// ==========================================================

tool('pcsx2_status', 'Connection and emulator status: running/paused, PCs, game title/serial/CRC. Also (re)connects.',
  {},
  async () => {
    const s = await dbg.call<Status>('status');
    if (!s.alive) return 'Connected. VM: not running (no game or BIOS booted).';
    return [
      `VM: ${s.paused ? 'paused' : 'running'}`,
      `Game: ${s.title || '(none)'}${s.serial ? ` [${s.serial}]` : ''}${s.disc_version ? ` v${s.disc_version}` : ''}  disc CRC ${s.disc_crc}, ELF CRC ${s.elf_crc}`,
      `EE PC: ${s.ee_pc}   IOP PC: ${s.iop_pc}   EE cycles: ${s.ee_cycles}`,
      `Breakpoints: ${s.breakpoints}   Watchpoints: ${s.watchpoints}`,
    ].join('\n');
  },
);

// ==========================================================
//  Memory
// ==========================================================

tool('pcsx2_read_memory', 'Read memory. Formats: hexdump, hex, or typed values (u8/s8/u16/s16/u32/s32/u64/f32/f64).',
  {
    address: addr(),
    length: z.number().int().min(1).max(65536).default(256).describe('Bytes to read'),
    format: z.enum(['hexdump', 'hex', 'u8', 's8', 'u16', 's16', 'u32', 's32', 'u64', 'f32', 'f64']).default('hexdump'),
    cpu: cpuArg(),
  },
  async ({ address, length, format, cpu }) => {
    const { addr: base, data } = await readMem(address, length, cpu);
    let body: string;
    if (format === 'hexdump') body = hexDump(data, base);
    else if (format === 'hex') body = data.toString('hex');
    else {
      const size = VALUE_SIZE[format];
      const rows: string[] = [];
      const perRow = Math.max(1, 16 / size);
      for (let i = 0; i + size <= data.length; i += size * perRow) {
        const vals: string[] = [];
        for (let k = i; k < i + size * perRow && k + size <= data.length; k += size) vals.push(formatValue(readValue(data, k, format), format));
        rows.push(`${hex32(base + i)}: ${vals.join(' ')}`);
      }
      body = rows.join('\n');
    }
    return `${length} bytes at ${hex32(base)}:\n${body}`;
  },
);

tool('pcsx2_write_memory', 'Write memory: either raw hex bytes ("data") or a typed value ("value" + "type").',
  {
    address: addr(),
    data: z.string().optional().describe('Hex bytes in memory order, e.g. "00 00 80 3f"'),
    value: z.string().optional().describe('Value to encode, e.g. "100", "0x3f800000", "1.5"'),
    type: z.enum(['u8', 's8', 'u16', 's16', 'u32', 's32', 'u64', 'f32', 'f64']).optional().describe('Type of "value"'),
    cpu: cpuArg(),
  },
  async ({ address, data, value, type, cpu }) => {
    let buf: Buffer;
    if (data !== undefined) {
      const clean = data.replace(/\s+/g, '');
      if (!/^([0-9a-f]{2})+$/i.test(clean)) throw new Error('"data" must be an even number of hex digits');
      buf = Buffer.from(clean, 'hex');
    } else if (value !== undefined && type) {
      buf = encodeValue(value, type);
    } else {
      throw new Error('pass "data", or "value" together with "type"');
    }
    const at = await writeMem(address, buf, cpu);
    return `Wrote ${buf.length} byte(s) at ${hex32(at)}: ${buf.toString('hex')}`;
  },
);

tool('pcsx2_read_string', 'Read a NUL-terminated string. Supports Shift-JIS, which most Japanese games use.',
  {
    address: addr(),
    max_length: z.number().int().min(1).max(65536).default(256),
    encoding: z.enum(['ascii', 'utf-8', 'shift_jis', 'latin1']).default('ascii'),
    cpu: cpuArg(),
  },
  async ({ address, max_length, encoding, cpu }) => {
    const r = await dbg.call<{ address: string; hex: string; terminated: boolean }>('read_string', { address, max_length, cpu });
    const base = parseInt(r.address, 16);
    const bytes = Buffer.from(r.hex, 'hex');
    const terminated = r.terminated;
    const str = encoding === 'ascii'
      ? bytes.toString('latin1').replace(/[^\x20-\x7e\n\t]/g, '.')
      : new TextDecoder(encoding).decode(bytes);
    return `${hex32(base)}: ${JSON.stringify(str)} (${bytes.length} bytes${terminated ? '' : ', no terminator found'})`;
  },
);

tool('pcsx2_find_pattern',
  'Search memory for bytes or a typed value. Default range is EE RAM above the kernel. '
  + 'Patterns are in memory order, which is little-endian: the opcode 0x27bdfff0 is "f0ffbd27".',
  {
    pattern: z.string().optional().describe('Hex bytes in memory order, "??" = any byte, e.g. "f0ffbd27 ????bfff"'),
    value: z.string().optional().describe('Alternatively a value to search for, e.g. "0x27bdfff0" or "1.5" (with "type")'),
    type: z.enum(['u8', 's8', 'u16', 's16', 'u32', 's32', 'u64', 'f32', 'f64']).default('u32'),
    start: addr().default('0x00100000'),
    end: addr().default('0x02000000'),
    align: z.number().int().min(1).max(16).default(1).describe('Only report matches at multiples of this'),
    max_results: z.number().int().min(1).max(10000).default(50),
    cpu: cpuArg(),
  },
  async ({ pattern: bytes, value, type, start, end, align, max_results, cpu }) => {
    if ((bytes === undefined) === (value === undefined)) throw new Error('pass exactly one of "pattern" or "value"');
    const pattern = bytes ?? encodeValue(value!, type).toString('hex');
    const { matches, truncated } = await dbg.call<{ matches: string[]; truncated: boolean }>(
      'find_memory', { pattern, start, end, align, max_results, cpu }, 120000);
    if (matches.length === 0) return `No matches for "${pattern}".`;
    return `${matches.length}${truncated ? '+' : ''} match(es):\n${matches.join('\n')}`;
  },
);

// Memory snapshots for finding values by how they change (cheat-search style).
interface Snapshot { addr: number; data: Buffer; candidates: number[] | null }
const snapshots = new Map<string, Snapshot>();

tool('pcsx2_memory_snapshot', 'Take a named memory snapshot for later comparison with pcsx2_memory_diff. Replaces any snapshot of the same name.',
  {
    name: z.string().default('default'),
    address: addr().default('0x00100000'),
    length: z.number().int().min(1).max(32 * 1024 * 1024).default(0x1f00000).describe('Bytes (default: all EE RAM above 1MB)'),
    cpu: cpuArg(),
  },
  async ({ name, address, length, cpu }) => {
    const { addr: base, data } = await readMem(address, length, cpu);
    snapshots.set(name, { addr: base, data, candidates: null });
    return `Snapshot "${name}": ${length} bytes at ${hex32(base)}.`;
  },
);

tool('pcsx2_memory_diff',
  'Compare memory against a snapshot. With narrow=true the matches become the candidate set for the next diff, so repeating '
  + '"change something in game → diff with filter increased/decreased/unchanged/equals" homes in on a variable.',
  {
    name: z.string().default('default'),
    type: z.enum(['u8', 's8', 'u16', 's16', 'u32', 's32', 'f32']).default('u8').describe('How to interpret each aligned element'),
    filter: z.enum(['changed', 'unchanged', 'increased', 'decreased', 'equals']).default('changed'),
    value: z.number().optional().describe('For filter=equals: the current value to match'),
    narrow: z.boolean().default(true).describe('Restrict future diffs of this snapshot to the matches'),
    update: z.boolean().default(true).describe('Replace the snapshot data with the current memory afterwards'),
    max_results: z.number().int().min(1).max(5000).default(100),
    cpu: cpuArg(),
  },
  async ({ name, type, filter, value, narrow, update, max_results, cpu }) => {
    const snap = snapshots.get(name);
    if (!snap) throw new Error(`no snapshot named "${name}" — take one with pcsx2_memory_snapshot`);
    if (filter === 'equals' && value === undefined) throw new Error('filter=equals needs "value"');
    const { data } = await readMem(snap.addr, snap.data.length, cpu);
    const size = VALUE_SIZE[type];
    const offsets = snap.candidates ?? Array.from({ length: Math.floor(data.length / size) }, (_, i) => i * size);
    const matches: number[] = [];
    for (const off of offsets) {
      const before = readValue(snap.data, off, type) as number;
      const now = readValue(data, off, type) as number;
      let ok: boolean;
      switch (filter) {
        case 'changed': ok = before !== now && !(Number.isNaN(before) && Number.isNaN(now)); break;
        case 'unchanged': ok = before === now || (Number.isNaN(before) && Number.isNaN(now)); break;
        case 'increased': ok = now > before; break;
        case 'decreased': ok = now < before; break;
        case 'equals': ok = type === 'f32' ? Math.abs(now - value!) < 1e-4 * Math.max(1, Math.abs(value!)) : now === value; break;
      }
      if (ok) matches.push(off);
    }
    const lines = matches.slice(0, max_results).map(off =>
      `  ${hex32(snap.addr + off)}: ${formatValue(readValue(snap.data, off, type), type)} → ${formatValue(readValue(data, off, type), type)}`);
    if (narrow) snap.candidates = matches;
    if (update) snap.data = data;
    return `${matches.length} of ${offsets.length} element(s) ${filter}${matches.length > max_results ? ` (showing ${max_results})` : ''}`
      + (narrow ? `; candidate set is now ${matches.length}.` : '.') + (lines.length ? '\n' + lines.join('\n') : '');
  },
);

// ==========================================================
//  Registers
// ==========================================================

tool('pcsx2_read_registers', 'Read registers. EE categories: GPR, CP0, FPR, FCR, VU0F, VU0I, GSPRIV (or "all"). IOP: GPR.',
  {
    category: z.string().default('GPR').describe('Category name or "all"'),
    cpu: cpuArg(),
  },
  async ({ category, cpu }) => {
        const r = await dbg.call<{ pc: string; hi: string; lo: string; categories: RegisterCategory[] }>(
      'read_registers', { cpu, category: category.toLowerCase() === 'all' ? undefined : category });
    const lines = [`${cpu.toUpperCase()} PC = ${r.pc}`];
    for (const cat of r.categories) {
      lines.push('', `--- ${cat.name} (${cat.bits}-bit) ---`);
      const width = Math.max(...cat.registers.map(x => x.name.length));
      for (const reg of cat.registers) lines.push(`${reg.name.padEnd(width)}  ${cat.name === 'GPR' ? formatGpr(reg.value) : reg.display}`);
    }
    return lines.join('\n');
  },
);

tool('pcsx2_write_register', 'Write a register by name (e.g. "v0", "f12", "pc" is not a register — use pcsx2_set_pc). VM must be paused.',
  {
    name: z.string().describe('Register name, e.g. "a0", "ra", "f0", "vf1"'),
    value: z.string().describe('Hex string up to 128 bits, e.g. "0x1234" or "3f800000"'),
    cpu: cpuArg(),
  },
  async ({ name, value, cpu }) => {
    const r = await dbg.call<{ register: string; value: string }>('write_register', { name, value, cpu });
    return `${r.register} = ${r.value}`;
  },
);

tool('pcsx2_set_pc', 'Set the program counter. VM must be paused.',
  { address: addr(), cpu: cpuArg() },
  async ({ address, cpu }) => {
    const r = await dbg.call<{ pc: string }>('set_pc', { value: address, cpu });
    return `PC = ${r.pc}`;
  },
);

// ==========================================================
//  Code
// ==========================================================

tool('pcsx2_disassemble', 'Disassemble with PCSX2\'s disassembler. Defaults to the current PC. Marks the PC (→), breakpoints (●) and function labels.',
  {
    address: addr().optional(),
    count: z.number().int().min(1).max(1000).default(24),
    before: z.number().int().min(0).max(500).default(0).describe('Also show this many instructions before the address'),
    cpu: cpuArg(),
  },
  async ({ address, count, before, cpu }) => {
    const r = await dbg.call<{ instructions: Instruction[] }>('disassemble', { address, count, before, cpu });
    const lines: string[] = [];
    for (const i of r.instructions) {
      if (i.label) lines.push(`${i.label}:`);
      lines.push(`${i.is_pc ? '→' : ' '}${i.breakpoint ? '●' : ' '} ${i.address}  ${i.opcode.slice(2)}  ${i.disasm}`);
    }
    return lines.join('\n') || 'No valid instructions at that address.';
  },
);

tool('pcsx2_evaluate', 'Evaluate a PCSX2 debugger expression: registers, symbols, arithmetic, comparisons, [addr] dereference. Bare numbers are hex.',
  { expression: z.string(), cpu: cpuArg() },
  async ({ expression, cpu }) => {
    const r = await dbg.call<{ value: number; hex: string; signed: number }>('evaluate', { expression, cpu });
    return `${expression} = ${r.hex} (${r.value}${r.signed < 0 ? `, s32 ${r.signed}` : ''})`;
  },
);

tool('pcsx2_lookup_symbol', 'Find a symbol by name, or the symbol containing an address. Symbols come from the ELF, .sym files and PCSX2\'s analysis.',
  { name: z.string().optional(), address: addr().optional(), cpu: cpuArg() },
  async ({ name, address, cpu }) => {
    if (name === undefined && address === undefined) throw new Error('pass "name" or "address"');
    const r = await dbg.call<{ name: string; address?: string; size: number; kind?: string; offset?: number }>(
      'lookup_symbol', { name, address, cpu });
    return `${r.name}${r.offset ? `+0x${r.offset.toString(16)}` : ''}: ${r.address ?? '?'} (${r.kind ?? 'symbol'}, ${r.size} bytes)`;
  },
);

tool('pcsx2_analyze', 'Re-run PCSX2\'s symbol import and function scan for the running ELF (happens automatically on connect). '
  + 'Useful after the game loads overlays. Runs in the background.',
  {},
  async () => {
    await dbg.call('analyze');
    return 'Analysis started; function names will appear in disassembly, backtraces and pcsx2_list_functions shortly.';
  },
);

tool('pcsx2_list_functions', 'List known functions, optionally filtered by name substring or address range.',
  {
    filter: z.string().optional(),
    start: addr().optional(),
    end: addr().optional(),
    limit: z.number().int().min(1).max(5000).default(100),
    cpu: cpuArg(),
  },
  async ({ filter, start, end, limit, cpu }) => {
    const r = await dbg.call<{ total: number; functions: Array<{ name: string; address: string; size: number }> }>(
      'list_functions', { filter, start, end, limit, cpu });
    const lines = r.functions.map(f => `${f.address}  ${String(f.size).padStart(6)}  ${f.name}`);
    return `${r.total} function(s)${r.total > r.functions.length ? `, showing ${r.functions.length}` : ''}:\n${lines.join('\n')}`;
  },
);

// ==========================================================
//  Breakpoints & watchpoints
// ==========================================================

const actionArg = () => z.enum(['break', 'log', 'both']).default('break')
  .describe('break = pause; log = count hits and log to the PCSX2 console without pausing; both = log and pause');

tool('pcsx2_set_breakpoint', 'Set (or replace) an execution breakpoint, optionally conditional.',
  {
    address: addr(),
    condition: z.string().optional().describe('Only break when this expression is non-zero, e.g. "a0 == 3" (hex numbers)'),
    action: actionArg(),
    log_format: z.string().optional().describe('For log/both, e.g. "a0={a0} [a1]={[a1]}"'),
    max_hits: z.number().int().min(0).default(0).describe('Disable after this many hits (0 = never)'),
    description: z.string().optional(),
    enabled: z.boolean().default(true),
    cpu: cpuArg(),
  },
  async ({ address, condition, action, log_format, max_hits, description, enabled, cpu }) => {
    const r = await dbg.call<{ address: string; function?: string }>('set_breakpoint',
      { address, condition, action, log_format, max_hits, description, enabled, cpu });
    return `Breakpoint at ${r.address}${r.function ? ` <${r.function}>` : ''}${condition ? ` if ${condition}` : ''}${action !== 'break' ? ` (${action})` : ''}`;
  },
);

tool('pcsx2_remove_breakpoint', 'Remove an execution breakpoint.',
  { address: addr(), cpu: cpuArg() },
  async ({ address, cpu }) => {
    const r = await dbg.call<{ address: string }>('remove_breakpoint', { address, cpu });
    return `Removed breakpoint at ${r.address}`;
  },
);

tool('pcsx2_enable_breakpoint', 'Enable or disable an execution breakpoint without removing it.',
  { address: addr(), enabled: z.boolean().default(true), cpu: cpuArg() },
  async ({ address, enabled, cpu }) => {
    const r = await dbg.call<{ address: string }>('enable_breakpoint', { address, enabled, cpu });
    return `Breakpoint at ${r.address} ${enabled ? 'enabled' : 'disabled'}`;
  },
);

tool('pcsx2_set_watchpoint', 'Set a memory watchpoint (breaks on read/write/access, or on writes that change the value).',
  {
    address: addr(),
    size: z.number().int().min(1).default(4).describe('Bytes to watch (ignored if "end" is given)'),
    end: addr().optional().describe('Exclusive end address'),
    type: z.enum(['read', 'write', 'readwrite', 'onchange']).default('write'),
    condition: z.string().optional(),
    action: actionArg(),
    log_format: z.string().optional(),
    max_hits: z.number().int().min(0).default(0),
    description: z.string().optional(),
    cpu: cpuArg(),
  },
  async ({ address, size, end, type, condition, action, log_format, max_hits, description, cpu }) => {
    const r = await dbg.call<{ start: string; end: string }>('set_memcheck',
      { address, size, end, type, condition, action, log_format, max_hits, description, cpu });
    return `Watchpoint (${type}${action !== 'break' ? `, ${action}` : ''}) on ${r.start}-${r.end}${condition ? ` if ${condition}` : ''}`;
  },
);

tool('pcsx2_remove_watchpoint', 'Remove a watchpoint (identified by its start address).',
  { address: addr(), cpu: cpuArg() },
  async ({ address, cpu }) => {
    const r = await dbg.call<{ start: string; end: string }>('remove_memcheck', { address, cpu });
    return `Removed watchpoint ${r.start}-${r.end}`;
  },
);

tool('pcsx2_enable_watchpoint', 'Enable or disable a watchpoint (identified by its start address).',
  { address: addr(), enabled: z.boolean().default(true), cpu: cpuArg() },
  async ({ address, enabled, cpu }) => {
    await dbg.call('enable_memcheck', { address, enabled, cpu });
    return `Watchpoint ${enabled ? 'enabled' : 'disabled'}`;
  },
);

tool('pcsx2_list_breakpoints', 'List breakpoints and watchpoints (both CPUs) with conditions and hit counts.',
  {},
  async () => {
    const r = await dbg.call<{ breakpoints: BreakpointInfo[]; watchpoints: WatchpointInfo[] }>('list_breakpoints');
    const lines: string[] = [];
    for (const b of r.breakpoints) {
      lines.push(`[bp] ${b.cpu} ${b.address}${b.function ? ` <${b.function}>` : ''} ${b.enabled ? 'on' : 'off'} hits=${b.hits}`
        + (b.max_hits ? `/${b.max_hits}` : '') + (b.condition ? ` if ${b.condition}` : '') + (b.action ? ` (${b.action})` : '')
        + (b.description ? ` — ${b.description}` : ''));
    }
    for (const w of r.watchpoints) {
      lines.push(`[wp] ${w.cpu} ${w.start}-${w.end} ${w.type} ${w.enabled ? 'on' : 'off'} hits=${w.hits}`
        + (w.last_pc ? ` last: PC ${w.last_pc} → ${w.last_access}` : '') + (w.condition ? ` if ${w.condition}` : '')
        + (w.action ? ` (${w.action})` : '') + (w.description ? ` — ${w.description}` : ''));
    }
    return lines.length ? lines.join('\n') : 'No breakpoints or watchpoints.';
  },
);

tool('pcsx2_clear_breakpoints', 'Remove ALL breakpoints and watchpoints on both CPUs.',
  {},
  async () => {
    await dbg.call('clear_breakpoints');
    return 'All breakpoints and watchpoints cleared.';
  },
);

// ==========================================================
//  Execution control
// ==========================================================

tool('pcsx2_pause', 'Pause the emulator and show where each CPU is.',
  { cpu: cpuArg() },
  async ({ cpu }) => {
    const r = await dbg.call<{ stop: Stop }>('pause');
    return formatStop(r.stop, cpu);
  },
);

tool('pcsx2_continue', 'Resume execution. With wait=true, block until a breakpoint/watchpoint fires or the timeout passes.',
  {
    wait: z.boolean().default(false),
    timeout_ms: z.number().int().min(0).max(600000).default(30000),
    cpu: cpuArg(),
  },
  async ({ wait, timeout_ms, cpu }) => {
        await dbg.call('resume');
    if (!wait) return 'Resumed.';
    const r = await dbg.call<{ timed_out: boolean; stop: Stop }>('wait', { timeout_ms }, timeout_ms + 10000);
    return r.timed_out ? `Still running after ${timeout_ms} ms (no breakpoint hit).` : formatStop(r.stop, cpu);
  },
);

tool('pcsx2_wait', 'Wait until the emulator pauses (breakpoint, watchpoint, frame advance) or the timeout passes.',
  { timeout_ms: z.number().int().min(0).max(600000).default(30000), cpu: cpuArg() },
  async ({ timeout_ms, cpu }) => {
    const r = await dbg.call<{ timed_out: boolean; stop: Stop }>('wait', { timeout_ms }, timeout_ms + 10000);
    return r.timed_out ? `Still running after ${timeout_ms} ms.` : formatStop(r.stop, cpu);
  },
);

tool('pcsx2_step', 'Step instructions (VM must be paused). into: follow calls; over: run calls to completion; out: run to the caller.',
  {
    mode: z.enum(['into', 'over', 'out']).default('into'),
    count: z.number().int().min(1).max(200).default(1),
    show_registers: z.boolean().default(false).describe('Show GPRs afterwards'),
    timeout_ms: z.number().int().min(1).max(600000).default(10000).describe('Per step'),
    cpu: cpuArg(),
  },
  async ({ mode, count, show_registers, timeout_ms, cpu }) => {
        const cmd = mode === 'into' ? 'step' : mode === 'over' ? 'step_over' : 'step_out';
    const lines: string[] = [];
    let last: RunResult | null = null;
    for (let i = 0; i < (mode === 'out' ? 1 : count); i++) {
      last = await dbg.call<RunResult>(cmd, { cpu, timeout_ms }, timeout_ms + 10000);
      const loc = cpu === 'iop' ? last.stop.iop : last.stop.ee;
      if (last.stop.reason !== 'step') {
        lines.push(formatStop(last.stop, cpu));
        if (last.timed_out) lines.push(`(target ${last.target} not reached within ${timeout_ms} ms; paused)`);
        break;
      }
      if (loc) lines.push(where(loc));
    }
    if (show_registers) {
      const r = await dbg.call<{ categories: RegisterCategory[] }>('read_registers', { cpu, category: 'GPR' });
      lines.push('', ...r.categories[0].registers.filter(x => x.name !== 'zero').map(x => `${x.name.padEnd(4)} ${formatGpr(x.value)}`));
    }
    return lines.join('\n');
  },
);

tool('pcsx2_run_to', 'Run until an address is reached (temporary breakpoint), another breakpoint fires, or the timeout passes. VM must be paused.',
  { address: addr(), timeout_ms: z.number().int().min(1).max(600000).default(30000), cpu: cpuArg() },
  async ({ address, timeout_ms, cpu }) => {
    const r = await dbg.call<RunResult>('run_to', { address, timeout_ms, cpu }, timeout_ms + 10000);
    return formatStop(r.stop, cpu) + (r.timed_out ? `\n(${r.target} not reached within ${timeout_ms} ms; paused)` : '');
  },
);

tool('pcsx2_frame_advance', 'Run a number of frames, then pause again. VM must be paused.',
  { frames: z.number().int().min(1).max(600).default(1), screenshot: z.boolean().default(false).describe('Return a screenshot afterwards') },
  async ({ frames, screenshot }) => {
        const r = await dbg.call<{ timed_out: boolean; stop: Stop }>('frame_advance', { frames }, 40000);
    const summary = r.timed_out ? 'Frame advance did not finish in time.' : `Advanced ${frames} frame(s).\n${formatStop(r.stop)}`;
    if (!screenshot) return summary;
    const shot = await dbg.call<{ png_base64: string }>('screenshot', { width: 640, height: 448 }, 30000);
    return { content: [{ type: 'text', text: summary }, { type: 'image', data: shot.png_base64, mimeType: 'image/png' }] };
  },
);

// ==========================================================
//  Controller input
// ==========================================================

const PAD_INPUTS = 'up, down, left, right, cross, circle, square, triangle, start, select, l1, l2, r1, r2, l3, r3, analog, '
  + 'and stick directions l_up/l_down/l_left/l_right, r_up/r_down/r_left/r_right';

tool('pcsx2_input',
  'Controller input on a DualShock 2. press: hold the inputs for some frames, then release (when paused this frame-advances '
  + 'and stays paused, so input is deterministic). hold: keep them held until released. release: let go of the given inputs, '
  + 'or of everything if none are given. Holding the PCSX2 window\'s bound keys/pad overrides injected input for those inputs.',
  {
    action: z.enum(['press', 'hold', 'release']).default('press'),
    inputs: z.string().default('').describe(`Comma-separated: ${PAD_INPUTS}. Append =0..1 for partial stick/pressure, e.g. "l_left=0.5,cross"`),
    frames: z.number().int().min(1).max(3600).default(4).describe('press: frames to hold'),
    port: z.number().int().min(1).max(2).default(1),
    screenshot: z.boolean().default(false).describe('press: return a screenshot afterwards'),
  },
  async ({ action, inputs, frames, port, screenshot }) => {
    if (action === 'hold') {
      await dbg.call('pad_set', { inputs, port });
      return `Holding ${inputs} on port ${port}.`;
    }
    if (action === 'release') {
      await dbg.call('pad_release', { inputs, port });
      return `Released ${inputs || 'all inputs'} on port ${port}.`;
    }
    const r = await dbg.call<{ stop?: Stop; timed_out?: boolean; frames_elapsed?: number }>(
      'pad_press', { inputs, port, frames }, 60000 + frames * 100);
    const summary = r.stop
      ? `Pressed ${inputs} for ${frames} frame(s).\n${r.timed_out ? 'Frame advance did not finish in time.' : formatStop(r.stop)}`
      : `Pressed ${inputs} for ${r.frames_elapsed} frame(s).`;
    if (!screenshot) return summary;
    const shot = await dbg.call<{ png_base64: string }>('screenshot', { width: 640, height: 448 }, 30000);
    return { content: [{ type: 'text', text: summary }, { type: 'image', data: shot.png_base64, mimeType: 'image/png' }] };
  },
);

// ==========================================================
//  System
// ==========================================================

tool('pcsx2_get_threads', 'List BIOS threads with status, wait reason, priority and entry point.',
  { cpu: cpuArg() },
  async ({ cpu }) => {
    const r = await dbg.call<{ threads: ThreadInfo[] }>('get_threads', { cpu });
    if (r.threads.length === 0) return 'No threads (the game may not have started yet).';
    return r.threads.map(t =>
      `TID ${String(t.id).padStart(3)}  ${t.status.padEnd(9)} prio ${String(t.priority).padStart(3)}  PC ${t.pc}  entry ${t.entry}`
      + (t.entry_function ? ` <${t.entry_function}>` : '') + (t.wait !== 'none' ? `  waiting on ${t.wait}${t.wait_id !== undefined ? ` #${t.wait_id}` : ''}` : ''),
    ).join('\n');
  },
);

tool('pcsx2_get_modules', 'List loaded IOP modules (IRX) with their addresses and sizes.',
  {},
  async () => {
    const r = await dbg.call<{ modules: ModuleInfo[] }>('get_modules');
    if (r.modules.length === 0) return 'No IOP modules loaded.';
    return r.modules.map(m => `${m.name.padEnd(24)} v${m.version.padEnd(6)} text ${m.text_addr} (+0x${m.text_size.toString(16)})  entry ${m.entry}`).join('\n');
  },
);

tool('pcsx2_get_backtrace', 'Call stack of the running thread (or a given thread), with function names where known.',
  {
    thread: z.number().int().optional().describe('Thread ID (from pcsx2_get_threads); default: the running thread'),
    max_frames: z.number().int().min(1).max(256).default(32),
    cpu: cpuArg(),
  },
  async ({ thread, max_frames, cpu }) => {
    const r = await dbg.call<{ frames: Frame[] }>('get_backtrace', { thread, max_frames, cpu });
    if (r.frames.length === 0) return 'No stack frames found.';
    return r.frames.map((f, i) => `#${String(i).padEnd(2)} ${where(f)}   (sp ${f.sp}, frame ${f.stack_size})`).join('\n');
  },
);

tool('pcsx2_screenshot', 'Capture the current frame as a PNG image (works while paused).',
  {
    save_path: z.string().optional().describe('Also write the PNG to this file'),
    width: z.number().int().min(0).max(4096).default(640).describe('Output size; 0×0 = full internal resolution'),
    height: z.number().int().min(0).max(4096).default(448),
  },
  async ({ save_path, width, height }) => {
    const r = await dbg.call<{ width: number; height: number; png_base64: string }>('screenshot', { width, height }, 30000);
    if (save_path) fs.writeFileSync(save_path, Buffer.from(r.png_base64, 'base64'));
    return {
      content: [
        { type: 'text', text: `${r.width}×${r.height}${save_path ? `, saved to ${save_path}` : ''}` },
        { type: 'image', data: r.png_base64, mimeType: 'image/png' },
      ],
    };
  },
);

tool('pcsx2_save_state', 'Save a savestate to a slot (1-10) or to a file path.',
  { slot: z.number().int().min(1).max(10).optional(), path: z.string().optional() },
  async ({ slot, path: file }) => {
    if ((slot === undefined) === (file === undefined)) throw new Error('pass exactly one of "slot" or "path"');
    await dbg.call('save_state', { slot, path: file && path.resolve(file) }, 60000);
    return `Saved state to ${file ? path.resolve(file) : `slot ${slot}`}.`;
  },
);

tool('pcsx2_load_state', 'Load a savestate from a slot (1-10) or a file path.',
  { slot: z.number().int().min(1).max(10).optional(), path: z.string().optional() },
  async ({ slot, path: file }) => {
    if ((slot === undefined) === (file === undefined)) throw new Error('pass exactly one of "slot" or "path"');
    const r = await dbg.call<{ paused: boolean; pc: string }>('load_state', { slot, path: file && path.resolve(file) }, 60000);
    return `Loaded ${file ?? `slot ${slot}`}; EE PC ${r.pc}, ${r.paused ? 'paused' : 'running'}.`;
  },
);

// ==========================================================
//  PS2Recomp integration (only when PS2RECOMP_ROOT is set)
// ==========================================================

if (PS2RECOMP_ROOT) {
  const overridesDir = path.join(PS2RECOMP_ROOT, 'ps2xRuntime', 'src', 'lib', 'overrides');

  tool('ps2recomp_lookup_function', 'Search the PS2Recomp project (overrides and TOML configs) for a function address or name.',
    { query: z.string() },
    async ({ query }) => {
      const q = query.toLowerCase();
      const results: string[] = [];
      if (fs.existsSync(overridesDir)) {
        for (const f of fs.readdirSync(overridesDir).filter(f => f.endsWith('.cpp'))) {
          if (!f.toLowerCase().includes(q.replace(/^0x/, ''))) continue;
          const m = fs.readFileSync(path.join(overridesDir, f), 'utf8').match(/RECOMP_FUNC\s+(\w+)/);
          results.push(`Override: ${f}${m ? ` → ${m[1]}` : ''}`);
        }
      }
      for (const d of ['configs', 'config']) {
        const dir = path.join(PS2RECOMP_ROOT, d);
        if (!fs.existsSync(dir)) continue;
        for (const f of fs.readdirSync(dir, { recursive: true }).filter((f): f is string => typeof f === 'string' && f.endsWith('.toml'))) {
          const lines = fs.readFileSync(path.join(dir, f), 'utf8').split('\n').filter(l => l.toLowerCase().includes(q));
          if (lines.length) results.push(`Config: ${f}`, ...lines.slice(0, 3).map(l => `  ${l.trim()}`));
        }
      }
      return results.length ? results.join('\n') : `No results for "${query}".`;
    },
  );

  tool('ps2recomp_list_overrides', 'List all PS2Recomp function overrides.',
    {},
    async () => {
      if (!fs.existsSync(overridesDir)) throw new Error(`override directory not found: ${overridesDir}`);
      const files = fs.readdirSync(overridesDir).filter(f => f.endsWith('.cpp')).sort();
      return `${files.length} overrides:\n` + files.map(f => { const m = f.match(/0x[0-9a-f]+/i); return `  ${(m?.[0] ?? '?').padEnd(12)} ${f}`; }).join('\n');
    },
  );
}

// ==========================================================
//  Resources
// ==========================================================

server.registerResource('ps2_memory_map', 'ps2://memory_map', { description: 'PS2 memory map', mimeType: 'text/plain' }, async (uri) => ({
  contents: [{
    uri: uri.href,
    mimeType: 'text/plain',
    text: [
      'EE (R5900) address space',
      '  0x00000000-0x01FFFFFF  RDRAM, 32MB (kernel below 0x00100000; mirrored uncached at 0x20000000, 0x30000000)',
      '  0x10000000-0x1000FFFF  EE hardware registers (timers, IPU, GIF, VIF, DMAC, INTC)',
      '  0x11000000-0x1100FFFF  VU0/VU1 micro and data memory',
      '  0x12000000-0x12001FFF  GS privileged registers',
      '  0x1C000000-0x1C1FFFFF  IOP RAM, 2MB (as seen from the EE)',
      '  0x1FC00000-0x1FFFFFFF  BIOS ROM, 4MB',
      '  0x70000000-0x70003FFF  Scratchpad RAM, 16KB',
      '  0x80000000-0x9FFFFFFF  kseg0 (cached RAM mirror)   0xA0000000-0xBFFFFFFF  kseg1 (uncached)',
      '',
      'IOP (R3000) address space',
      '  0x00000000-0x001FFFFF  IOP RAM, 2MB',
      '  0x1F800000-0x1F8003FF  IOP scratchpad',
      '  0x1F801000-0x1F80FFFF  IOP hardware registers',
      '  0x1FC00000-0x1FFFFFFF  BIOS ROM',
    ].join('\n'),
  }],
}));

// ==========================================================
//  Main
// ==========================================================

await server.connect(new StdioServerTransport());
console.error(`PCSX2 MCP server running (DebugServer 127.0.0.1:${dbg.port})`);
