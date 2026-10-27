/**
 * Client for the DebugServer patched into PCSX2 (pcsx2-plugin/DebugServer.cpp).
 *
 * Protocol: newline-delimited JSON over TCP (127.0.0.1:21512).
 *   Request:  {"cmd":"...","id":"7",...}\n
 *   Response: {"ok":true,"id":"7",...}\n  or  {"ok":false,"id":"7","error":"..."}\n
 *
 * The server answers requests on a connection in order, so pending requests
 * are kept in a FIFO; the id is only used as a sanity check.
 */

import * as net from 'node:net';

export type Cpu = 'ee' | 'iop';

export interface Location {
  pc: string;
  function?: string;
  function_offset?: number;
  disasm: string;
}

export interface Hit {
  type: 'breakpoint' | 'watchpoint';
  cpu: Cpu;
  address: string;
  end?: string;
  last_pc?: string;
  last_access?: string;
  last_size?: number;
  new_hits: number;
}

export interface Stop {
  paused: boolean;
  alive: boolean;
  reason: 'breakpoint' | 'step' | 'run_to' | 'paused' | 'timeout' | 'frame_advance' | 'running';
  hits: Hit[];
  ee?: Location;
  iop?: Location;
}

export interface RunResult {
  from?: string;
  target?: string;
  timed_out?: boolean;
  stop: Stop;
}

export interface Status {
  alive: boolean;
  paused?: boolean;
  ee_pc?: string;
  iop_pc?: string;
  ee_cycles?: number;
  title?: string;
  serial?: string;
  disc_version?: string;
  disc_crc?: string;
  elf_crc?: string;
  breakpoints?: number;
  watchpoints?: number;
}

export interface RegisterCategory {
  name: string;
  index: number;
  bits: number;
  registers: Array<{ name: string; value: string; display: string }>;
}

export interface Instruction {
  address: string;
  opcode: string;
  disasm: string;
  label?: string;
  is_pc?: boolean;
  breakpoint?: boolean;
}

export interface BreakpointInfo {
  cpu: Cpu;
  address: string;
  enabled: boolean;
  temporary?: boolean;
  condition?: string;
  description?: string;
  hits: number;
  max_hits?: number;
  action?: 'log' | 'both';
  function?: string;
}

export interface WatchpointInfo {
  cpu: Cpu;
  start: string;
  end: string;
  type: 'read' | 'write' | 'readwrite' | 'onchange';
  enabled: boolean;
  condition?: string;
  description?: string;
  hits: number;
  last_pc?: string;
  last_access?: string;
  action?: 'log' | 'both';
}

export interface ThreadInfo {
  id: number;
  status: string;
  wait: string;
  wait_id?: number;
  priority: number;
  entry: string;
  entry_function?: string;
  pc: string;
}

export interface ModuleInfo {
  name: string;
  version: string;
  text_addr: string;
  entry: string;
  gp: string;
  text_size: number;
  data_size: number;
  bss_size: number;
}

export interface Frame extends Location {
  entry: string;
  entry_function?: string;
  sp: string;
  stack_size: number;
}

export class DebugServerError extends Error {}

interface Pending {
  id: string;
  cmd: string;
  resolve: (value: any) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
}

export class DebugServerClient {
  private socket: net.Socket | null = null;
  private connecting: Promise<void> | null = null;
  private buffer = '';
  private pending: Pending[] = [];
  private nextId = 1;

  constructor(readonly host = '127.0.0.1', readonly port = 21512) {}

  isConnected(): boolean {
    return this.socket !== null && !this.socket.destroyed && this.connecting === null;
  }

  /** Connects if needed. Safe to call concurrently. */
  async ensureConnected(): Promise<void> {
    if (this.isConnected()) return;
    if (!this.connecting) {
      this.connecting = this.open().finally(() => { this.connecting = null; });
    }
    return this.connecting;
  }

  private open(): Promise<void> {
    return new Promise((resolve, reject) => {
      const socket = net.createConnection({ host: this.host, port: this.port });
      socket.setEncoding('utf8');
      socket.setNoDelay(true);

      const timer = setTimeout(() => {
        socket.destroy();
        reject(new Error(`timed out connecting to the PCSX2 DebugServer at ${this.host}:${this.port}`));
      }, 3000);

      socket.once('connect', () => {
        clearTimeout(timer);
        this.socket = socket;
        this.buffer = '';
        resolve();
      });
      socket.on('data', (chunk: string) => this.onData(chunk));
      socket.on('error', (err: NodeJS.ErrnoException) => {
        clearTimeout(timer);
        if (this.socket !== socket) {
          reject(err.code === 'ECONNREFUSED'
            ? new Error(`PCSX2 DebugServer is not listening on ${this.host}:${this.port} — is the patched PCSX2 running?`)
            : err);
        }
      });
      socket.on('close', () => {
        if (this.socket === socket) this.socket = null;
        this.failAll(new Error('connection to PCSX2 DebugServer closed'));
      });
    });
  }

  disconnect(): void {
    this.socket?.destroy();
    this.socket = null;
  }

  private failAll(err: Error): void {
    for (const p of this.pending.splice(0)) {
      clearTimeout(p.timer);
      p.reject(err);
    }
  }

  private onData(chunk: string): void {
    this.buffer += chunk;
    let nl: number;
    while ((nl = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, nl);
      this.buffer = this.buffer.slice(nl + 1);
      const p = this.pending.shift();
      if (!p) continue;
      clearTimeout(p.timer);
      let msg: any;
      try {
        msg = JSON.parse(line);
      } catch {
        p.reject(new Error(`invalid JSON from DebugServer: ${line.slice(0, 200)}`));
        continue;
      }
      if (msg.id !== undefined && msg.id !== p.id) {
        p.reject(new Error(`DebugServer response out of sync (expected id ${p.id}, got ${msg.id})`));
        this.disconnect();
        return;
      }
      if (msg.ok) p.resolve(msg);
      else p.reject(new DebugServerError(msg.error ?? `${p.cmd} failed`));
    }
  }

  /**
   * Sends a command and resolves with the response object (minus ok/id).
   * `timeoutMs` should exceed any server-side wait the command performs.
   */
  async call<T = any>(cmd: string, params: Record<string, unknown> = {}, timeoutMs = 15000): Promise<T> {
    await this.ensureConnected();
    const socket = this.socket!;
    const id = String(this.nextId++);
    const body: Record<string, unknown> = { cmd, id };
    for (const [k, v] of Object.entries(params)) if (v !== undefined) body[k] = v;

    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        // The response may still arrive and would desync the FIFO; drop the connection.
        reject(new Error(`DebugServer command '${cmd}' timed out after ${timeoutMs} ms`));
        this.disconnect();
      }, timeoutMs);
      this.pending.push({
        id,
        cmd,
        timer,
        reject,
        resolve: (msg) => {
          delete msg.ok;
          delete msg.id;
          resolve(msg as T);
        },
      });
      socket.write(JSON.stringify(body) + '\n');
    });
  }
}
