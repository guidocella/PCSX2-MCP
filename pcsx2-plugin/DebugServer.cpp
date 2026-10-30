// SPDX-FileCopyrightText: 2026 PS2Recomp Debug Bridge
// SPDX-License-Identifier: GPL-3.0+
//
// PCSX2 Debug Server — newline-delimited JSON over TCP (127.0.0.1:21512)
//
// Request:  {"cmd":"read_registers","cpu":"ee","category":"GPR"}\n
// Response: {"ok":true,...}\n   or   {"ok":false,"error":"..."}\n
//
// Address-like parameters ("address", "end", "value" of set_pc, ...) accept
// either a JSON number or a string, which is evaluated with PCSX2's expression
// parser. Bare numbers in expressions are hexadecimal, and registers and symbol
// names are allowed, e.g. "0x1000e0", "a0+10", "main".
//
// Threading: reads go straight to the DebugInterface like the Qt debugger
// does. Anything that mutates emulator state (breakpoints, registers, memory,
// pause/resume, savestates) runs on the CPU thread, since breakpoint changes
// reset the recompilers and VM state transitions must happen there.
//
// Integration:
//   1. Put DebugServer.h + DebugServer.cpp in pcsx2/DebugTools/ and add them
//      to pcsx2/CMakeLists.txt
//   2. Call DebugServer::Start() from VMManager::Internal::CPUThreadInitialize()
//   3. Call DebugServer::Stop() from VMManager::Internal::CPUThreadShutdown()

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#define SEND_FLAGS 0
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET close
#define SEND_FLAGS MSG_NOSIGNAL
#endif

#include "DebugServer.h"
#include "DebugInterface.h"
#include "Breakpoints.h"
#include "MIPSAnalyst.h"
#include "MipsStackWalk.h"
#include "SymbolImporter.h"

#include "Config.h"
#include "Counters.h"
#include "Host.h"
#include "MTGS.h"
#include "VMManager.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"

#include "common/Error.h"
#include "common/Image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace DebugServer
{
	// ============================================================
	// JSON writer
	// ============================================================
	class JsonWriter
	{
	public:
		JsonWriter& beginObject() { prefix(); m_buf += '{'; m_first.push_back(true); return *this; }
		JsonWriter& endObject() { m_buf += '}'; m_first.pop_back(); return *this; }
		JsonWriter& beginArray() { prefix(); m_buf += '['; m_first.push_back(true); return *this; }
		JsonWriter& endArray() { m_buf += ']'; m_first.pop_back(); return *this; }

		JsonWriter& key(const char* k)
		{
			prefix();
			quoted(k);
			m_buf += ':';
			m_afterKey = true;
			return *this;
		}

		JsonWriter& str(std::string_view v) { prefix(); quoted(v); return *this; }
		JsonWriter& num(int64_t v) { prefix(); m_buf += std::to_string(v); return *this; }
		JsonWriter& unum(uint64_t v) { prefix(); m_buf += std::to_string(v); return *this; }
		JsonWriter& boolean(bool v) { prefix(); m_buf += v ? "true" : "false"; return *this; }
		JsonWriter& null() { prefix(); m_buf += "null"; return *this; }
		JsonWriter& raw(std::string_view v) { prefix(); m_buf += v; return *this; }

		JsonWriter& hex32(u32 v)
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "0x%08x", v);
			return str(buf);
		}

		JsonWriter& hex128(const u128& v)
		{
			char buf[40];
			std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", v._u32[3], v._u32[2], v._u32[1], v._u32[0]);
			return str(buf);
		}

		const std::string& result() const { return m_buf; }

	private:
		void prefix()
		{
			if (m_afterKey)
			{
				m_afterKey = false;
				return;
			}
			if (!m_first.empty())
			{
				if (!m_first.back())
					m_buf += ',';
				m_first.back() = false;
			}
		}

		void quoted(std::string_view s)
		{
			m_buf += '"';
			for (const char ch : s)
			{
				const unsigned char c = static_cast<unsigned char>(ch);
				switch (c)
				{
					case '"': m_buf += "\\\""; break;
					case '\\': m_buf += "\\\\"; break;
					case '\n': m_buf += "\\n"; break;
					case '\r': m_buf += "\\r"; break;
					case '\t': m_buf += "\\t"; break;
					default:
						if (c < 0x20 || c == 0x7f)
						{
							char esc[8];
							std::snprintf(esc, sizeof(esc), "\\u%04x", c);
							m_buf += esc;
						}
						else
						{
							m_buf += ch;
						}
						break;
				}
			}
			m_buf += '"';
		}

		std::string m_buf;
		std::vector<bool> m_first;
		bool m_afterKey = false;
	};

	// ============================================================
	// JSON request parser — a flat object of scalars. Nested values
	// are skipped. Strings are fully unescaped (including \uXXXX).
	// ============================================================
	struct JsonValue
	{
		enum Type { NONE, STRING, NUMBER, BOOL, NUL } type = NONE;
		std::string s;
		double n = 0;
		bool b = false;
	};

	class JsonParser
	{
	public:
		explicit JsonParser(std::string_view text) : m_t(text) {}

		bool parseObject(std::unordered_map<std::string, JsonValue>& out, std::string& error)
		{
			ws();
			if (!eat('{'))
				return fail(error, "expected '{'");
			ws();
			if (eat('}'))
				return true;
			for (;;)
			{
				ws();
				std::string k;
				if (!parseString(k))
					return fail(error, "expected string key");
				ws();
				if (!eat(':'))
					return fail(error, "expected ':'");
				ws();
				JsonValue v;
				if (!parseValue(v))
					return fail(error, "invalid value for key '" + k + "'");
				out[k] = std::move(v);
				ws();
				if (eat(','))
					continue;
				if (eat('}'))
					return true;
				return fail(error, "expected ',' or '}'");
			}
		}

	private:
		bool fail(std::string& error, const std::string& msg)
		{
			error = "JSON parse error at offset " + std::to_string(m_i) + ": " + msg;
			return false;
		}

		void ws()
		{
			while (m_i < m_t.size() && (m_t[m_i] == ' ' || m_t[m_i] == '\t' || m_t[m_i] == '\n' || m_t[m_i] == '\r'))
				m_i++;
		}

		bool eat(char c)
		{
			if (m_i < m_t.size() && m_t[m_i] == c)
			{
				m_i++;
				return true;
			}
			return false;
		}

		bool literal(std::string_view word)
		{
			if (m_t.substr(m_i, word.size()) != word)
				return false;
			m_i += word.size();
			return true;
		}

		static void appendUtf8(std::string& out, u32 cp)
		{
			if (cp < 0x80)
				out += static_cast<char>(cp);
			else if (cp < 0x800)
			{
				out += static_cast<char>(0xC0 | (cp >> 6));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else if (cp < 0x10000)
			{
				out += static_cast<char>(0xE0 | (cp >> 12));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | (cp >> 18));
				out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
		}

		bool hex4(u32& out)
		{
			if (m_i + 4 > m_t.size())
				return false;
			out = 0;
			for (int k = 0; k < 4; k++)
			{
				const char c = m_t[m_i++];
				out <<= 4;
				if (c >= '0' && c <= '9') out |= c - '0';
				else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
				else return false;
			}
			return true;
		}

		bool parseString(std::string& out)
		{
			if (!eat('"'))
				return false;
			while (m_i < m_t.size())
			{
				const char c = m_t[m_i++];
				if (c == '"')
					return true;
				if (c != '\\')
				{
					out += c;
					continue;
				}
				if (m_i >= m_t.size())
					return false;
				const char e = m_t[m_i++];
				switch (e)
				{
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u':
					{
						u32 cp;
						if (!hex4(cp))
							return false;
						if (cp >= 0xD800 && cp < 0xDC00 && literal("\\u"))
						{
							u32 lo;
							if (!hex4(lo))
								return false;
							cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						}
						appendUtf8(out, cp);
						break;
					}
					default: return false;
				}
			}
			return false;
		}

		bool skipNested()
		{
			int depth = 0;
			while (m_i < m_t.size())
			{
				const char c = m_t[m_i];
				if (c == '"')
				{
					std::string dummy;
					if (!parseString(dummy))
						return false;
					continue;
				}
				m_i++;
				if (c == '{' || c == '[')
					depth++;
				else if ((c == '}' || c == ']') && --depth == 0)
					return true;
			}
			return false;
		}

		bool parseValue(JsonValue& v)
		{
			if (m_i >= m_t.size())
				return false;
			const char c = m_t[m_i];
			if (c == '"')
			{
				v.type = JsonValue::STRING;
				return parseString(v.s);
			}
			if (c == '{' || c == '[')
				return skipNested();
			if (literal("true"))
			{
				v.type = JsonValue::BOOL;
				v.b = true;
				return true;
			}
			if (literal("false"))
			{
				v.type = JsonValue::BOOL;
				return true;
			}
			if (literal("null"))
			{
				v.type = JsonValue::NUL;
				return true;
			}
			const size_t start = m_i;
			while (m_i < m_t.size() && std::strchr("+-0123456789.eE", m_t[m_i]))
				m_i++;
			if (m_i == start)
				return false;
			v.type = JsonValue::NUMBER;
			v.n = std::strtod(std::string(m_t.substr(start, m_i - start)).c_str(), nullptr);
			return true;
		}

		std::string_view m_t;
		size_t m_i = 0;
	};

	// ============================================================
	// Request / response plumbing
	// ============================================================
	class Request
	{
	public:
		std::unordered_map<std::string, JsonValue> params;

		bool has(const char* k) const
		{
			const auto it = params.find(k);
			return it != params.end() && it->second.type != JsonValue::NONE && it->second.type != JsonValue::NUL;
		}

		std::string str(const char* k, const char* def = "") const
		{
			const auto it = params.find(k);
			if (it == params.end())
				return def;
			if (it->second.type == JsonValue::STRING)
				return it->second.s;
			if (it->second.type == JsonValue::NUMBER)
				return std::to_string(static_cast<int64_t>(it->second.n));
			return def;
		}

		int64_t integer(const char* k, int64_t def = 0) const
		{
			const auto it = params.find(k);
			if (it == params.end())
				return def;
			if (it->second.type == JsonValue::NUMBER)
				return static_cast<int64_t>(it->second.n);
			if (it->second.type == JsonValue::STRING && !it->second.s.empty())
				return static_cast<int64_t>(std::strtoll(it->second.s.c_str(), nullptr, 0));
			return def;
		}

		bool boolean(const char* k, bool def = false) const
		{
			const auto it = params.find(k);
			if (it != params.end() && it->second.type == JsonValue::BOOL)
				return it->second.b;
			return def;
		}
	};

	// Handlers write their payload fields into `out` (inside the top-level
	// object) and return true, or set `error` and return false.
	struct Context
	{
		const Request& req;
		DebugInterface* cpu;
		BreakPointCpu bpCpu;
		JsonWriter& out;
		std::string& error;

		bool fail(std::string msg)
		{
			error = std::move(msg);
			return false;
		}
	};

	using Handler = bool (*)(Context&);

	static bool equalsNoCase(const char* a, const char* b)
	{
		for (; *a && *b; a++, b++)
		{
			if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
				return false;
		}
		return *a == *b;
	}

	static void runOnCpuThread(std::function<void()> fn)
	{
		Host::RunOnCPUThread(std::move(fn), true);
	}

	// Evaluates an address/value parameter: JSON numbers are taken verbatim,
	// strings go through the expression parser (hex by default, registers and
	// symbols allowed).
	static bool getAddress(Context& c, const char* key, u32& out, std::optional<u32> def = std::nullopt)
	{
		const auto it = c.req.params.find(key);
		if (it == c.req.params.end() || it->second.type == JsonValue::NUL)
		{
			if (def)
			{
				out = *def;
				return true;
			}
			return c.fail(std::string("missing parameter '") + key + "'");
		}
		if (it->second.type == JsonValue::NUMBER)
		{
			out = static_cast<u32>(static_cast<int64_t>(it->second.n));
			return true;
		}
		if (it->second.type != JsonValue::STRING)
			return c.fail(std::string("parameter '") + key + "' must be a number or expression string");

		u64 value = 0;
		std::string error;
		if (!c.cpu->evaluateExpression(it->second.s.c_str(), value, error))
			return c.fail(std::string("cannot evaluate '") + key + "' (" + it->second.s + "): " + error);
		out = static_cast<u32>(value);
		return true;
	}

	static bool requireVM(Context& c)
	{
		if (!c.cpu->isAlive())
			return c.fail("no VM running — boot a game or the BIOS in PCSX2 first");
		return true;
	}

	static bool requirePaused(Context& c)
	{
		if (!requireVM(c))
			return false;
		if (!c.cpu->isCpuPaused())
			return c.fail("the VM must be paused (use pause first)");
		return true;
	}

	static std::string functionNameAt(DebugInterface* cpu, u32 address, bool overlapping, u32* offset = nullptr)
	{
		const FunctionInfo fn = overlapping ?
			cpu->GetSymbolGuardian().FunctionOverlappingAddress(address) :
			cpu->GetSymbolGuardian().FunctionStartingAtAddress(address);
		if (!fn.address.valid())
			return {};
		if (offset)
			*offset = address - fn.address.value;
		return fn.name;
	}

	static void writeLocation(JsonWriter& j, DebugInterface* cpu, u32 pc)
	{
		j.key("pc").hex32(pc);
		u32 offset = 0;
		const std::string fn = functionNameAt(cpu, pc, true, &offset);
		if (!fn.empty())
		{
			j.key("function").str(fn);
			j.key("function_offset").unum(offset);
		}
		j.key("disasm").str(cpu->isValidAddress(pc) ? cpu->disasm(pc, true) : std::string("<invalid address>"));
	}

	// ============================================================
	// Stop tracking: we remember the hit counters of every breakpoint
	// and memcheck when we resume the VM, so that when it stops again we
	// can tell the client which one(s) fired. This works whether or not
	// the Qt debugger window is open (it clears the "triggered" flag).
	// ============================================================
	struct HitKey
	{
		BreakPointCpu cpu;
		bool memcheck;
		u32 start;
		u32 end;
		bool operator<(const HitKey& o) const
		{
			return std::tie(cpu, memcheck, start, end) < std::tie(o.cpu, o.memcheck, o.start, o.end);
		}
	};

	static std::mutex s_hitMutex;
	static std::vector<std::pair<HitKey, u32>> s_hitSnapshot;

	static std::vector<std::pair<HitKey, u32>> collectHits()
	{
		std::vector<std::pair<HitKey, u32>> hits;
		for (const BreakPointCpu cpu : DEBUG_CPUS)
		{
			for (const BreakPoint& bp : CBreakPoints::GetBreakpoints(cpu, false))
				hits.push_back({{cpu, false, bp.addr, bp.addr}, bp.totalHits});
			for (const MemCheck& mc : CBreakPoints::GetMemChecks(cpu))
				hits.push_back({{cpu, true, mc.start, mc.end}, mc.totalHits});
		}
		return hits;
	}

	// Must run on the CPU thread.
	static void resumeVM()
	{
		{
			std::lock_guard lock(s_hitMutex);
			s_hitSnapshot = collectHits();
		}
		// Step past a breakpoint we might be sitting on, like the Qt debugger does.
		CBreakPoints::SetSkipFirst(BREAKPOINT_EE, r5900Debug.getPC());
		CBreakPoints::SetSkipFirst(BREAKPOINT_IOP, r3000Debug.getPC());
		r5900Debug.resumeCpu();
	}

	// Must run on the CPU thread. Mirrors DebuggerWindow::onVMPaused so that
	// temporary (stepping) breakpoints don't accumulate when the Qt debugger
	// isn't open.
	static void cleanupAfterStop()
	{
		CBreakPoints::ClearTemporaryBreakPoints();
		CBreakPoints::SetBreakpointTriggered(false, BREAKPOINT_IOP_AND_EE);
	}

	static bool waitForPause(int timeout_ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		while (VMManager::HasValidVM() && !r5900Debug.isCpuPaused())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return true;
	}

	// Writes a "stop" object describing why/where the VM stopped.
	static void writeStop(JsonWriter& j, const char* reason)
	{
		std::vector<std::pair<HitKey, u32>> before;
		{
			std::lock_guard lock(s_hitMutex);
			before = s_hitSnapshot;
		}
		const auto now = collectHits();

		j.key("stop").beginObject();
		j.key("paused").boolean(r5900Debug.isCpuPaused());
		j.key("alive").boolean(r5900Debug.isAlive());

		j.key("hits").beginArray();
		bool anyHit = false;
		for (const auto& [key, count] : now)
		{
			u32 prev = 0;
			for (const auto& [bkey, bcount] : before)
			{
				if (!(bkey < key) && !(key < bkey))
				{
					prev = bcount;
					break;
				}
			}
			if (count <= prev)
				continue;
			anyHit = true;
			j.beginObject();
			j.key("type").str(key.memcheck ? "watchpoint" : "breakpoint");
			j.key("cpu").str(key.cpu == BREAKPOINT_IOP ? "iop" : "ee");
			j.key("address").hex32(key.start);
			if (key.memcheck)
			{
				j.key("end").hex32(key.end);
				for (const MemCheck& mc : CBreakPoints::GetMemChecks(key.cpu))
				{
					// Only the interpreter records the access; recompiled code just counts hits.
					if (mc.start == key.start && mc.end == key.end && mc.lastSize != 0)
					{
						j.key("last_pc").hex32(mc.lastPC);
						j.key("last_access").hex32(mc.lastAddr);
						j.key("last_size").num(mc.lastSize);
					}
				}
			}
			j.key("new_hits").unum(count - prev);
			j.endObject();
		}
		j.endArray();

		const bool paused = r5900Debug.isAlive() && r5900Debug.isCpuPaused();
		j.key("reason").str(!paused ? "running" : anyHit ? "breakpoint" : reason);

		if (r5900Debug.isAlive())
		{
			j.key("ee").beginObject();
			writeLocation(j, &r5900Debug, r5900Debug.getPC());
			j.endObject();
			j.key("iop").beginObject();
			writeLocation(j, &r3000Debug, r3000Debug.getPC());
			j.endObject();
		}
		j.endObject();
	}

	// Runs the CPU until it reaches `target` on the requested CPU (via a temporary
	// stepping breakpoint), another breakpoint fires, or the timeout expires.
	static bool runUntil(Context& c, u32 target, const char* reason)
	{
		const int timeout = static_cast<int>(std::clamp<int64_t>(c.req.integer("timeout_ms", 10000), 1, 600000));
		const u32 startPc = c.cpu->getPC();
		const BreakPointCpu bpCpu = c.bpCpu;

		runOnCpuThread([bpCpu, target] {
			CBreakPoints::AddBreakPoint(bpCpu, target, true, true, true);
			resumeVM();
		});

		const bool stopped = waitForPause(timeout);
		if (!stopped)
		{
			runOnCpuThread([] { r5900Debug.pauseCpu(); });
			waitForPause(2000);
		}
		runOnCpuThread(cleanupAfterStop);

		c.out.key("from").hex32(startPc);
		c.out.key("target").hex32(target);
		c.out.key("timed_out").boolean(!stopped);
		writeStop(c.out, stopped ? (c.cpu->getPC() == target ? reason : "paused") : "timeout");
		return true;
	}

	// ============================================================
	// Command handlers
	// ============================================================

	static bool cmdPing(Context& c)
	{
		c.out.key("server").str("pcsx2-debug-server");
		c.out.key("protocol").num(2);
		return true;
	}

	static bool cmdStatus(Context& c)
	{
		const bool alive = c.cpu->isAlive();
		c.out.key("alive").boolean(alive);
		if (!alive)
			return true;
		c.out.key("paused").boolean(c.cpu->isCpuPaused());
		c.out.key("ee_pc").hex32(r5900Debug.getPC());
		c.out.key("iop_pc").hex32(r3000Debug.getPC());
		c.out.key("ee_cycles").unum(r5900Debug.getCycles());
		c.out.key("title").str(VMManager::GetTitle(true));
		c.out.key("serial").str(VMManager::GetDiscSerial());
		c.out.key("disc_version").str(VMManager::GetDiscVersion());
		char crc[16];
		std::snprintf(crc, sizeof(crc), "%08X", VMManager::GetDiscCRC());
		c.out.key("disc_crc").str(crc);
		std::snprintf(crc, sizeof(crc), "%08X", VMManager::GetCurrentCRC());
		c.out.key("elf_crc").str(crc);
		c.out.key("breakpoints").unum(CBreakPoints::GetNumBreakpoints());
		c.out.key("watchpoints").unum(CBreakPoints::GetNumMemchecks());
		return true;
	}

	static bool resolveCategory(Context& c, int& cat)
	{
		cat = -1;
		if (!c.req.has("category"))
			return true;
		const auto& v = c.req.params.at("category");
		if (v.type == JsonValue::NUMBER)
			cat = static_cast<int>(v.n);
		else
		{
			for (int i = 0; i < c.cpu->getRegisterCategoryCount(); i++)
			{
				if (equalsNoCase(c.cpu->getRegisterCategoryName(i), v.s.c_str()))
					cat = i;
			}
			if (cat < 0)
				return c.fail("unknown register category '" + v.s + "'");
		}
		if (cat < -1 || cat >= c.cpu->getRegisterCategoryCount())
			return c.fail("register category out of range");
		return true;
	}

	static bool cmdReadRegisters(Context& c)
	{
		if (!requireVM(c))
			return false;
		int cat;
		if (!resolveCategory(c, cat))
			return false;

		const int first = cat >= 0 ? cat : 0;
		const int last = cat >= 0 ? cat + 1 : c.cpu->getRegisterCategoryCount();

		c.out.key("pc").hex32(c.cpu->getPC());
		c.out.key("hi").hex128(c.cpu->getHI());
		c.out.key("lo").hex128(c.cpu->getLO());
		c.out.key("categories").beginArray();
		for (int i = first; i < last; i++)
		{
			c.out.beginObject();
			c.out.key("name").str(c.cpu->getRegisterCategoryName(i));
			c.out.key("index").num(i);
			c.out.key("bits").num(c.cpu->getRegisterSize(i));
			c.out.key("registers").beginArray();
			for (int r = 0; r < c.cpu->getRegisterCount(i); r++)
			{
				c.out.beginObject();
				c.out.key("name").str(c.cpu->getRegisterName(i, r));
				c.out.key("value").hex128(c.cpu->getRegister(i, r));
				c.out.key("display").str(c.cpu->getRegisterString(i, r));
				c.out.endObject();
			}
			c.out.endArray();
			c.out.endObject();
		}
		c.out.endArray();
		return true;
	}

	static bool parseU128(const std::string& text, u128& out)
	{
		std::string s = text;
		if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			s = s.substr(2);
		if (s.empty() || s.size() > 32 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
			return false;
		s.insert(0, 32 - s.size(), '0');
		for (int i = 0; i < 4; i++)
			out._u32[i] = static_cast<u32>(std::strtoul(s.substr(24 - i * 8, 8).c_str(), nullptr, 16));
		return true;
	}

	static bool cmdWriteRegister(Context& c)
	{
		if (!requirePaused(c))
			return false;

		int cat = -1, num = -1;
		const std::string name = c.req.str("name");
		if (!name.empty())
		{
			for (int i = 0; i < c.cpu->getRegisterCategoryCount() && cat < 0; i++)
			{
				for (int r = 0; r < c.cpu->getRegisterCount(i); r++)
				{
					if (equalsNoCase(c.cpu->getRegisterName(i, r), name.c_str()))
					{
						cat = i;
						num = r;
						break;
					}
				}
			}
			if (cat < 0)
				return c.fail("unknown register '" + name + "'");
		}
		else
		{
			if (!resolveCategory(c, cat))
				return false;
			if (cat < 0)
				cat = 0;
			num = static_cast<int>(c.req.integer("index", -1));
			if (num < 0 || num >= c.cpu->getRegisterCount(cat))
				return c.fail("register index out of range (or pass 'name')");
		}

		u128 value = {};
		const auto& v = c.req.params.find("value");
		if (v == c.req.params.end())
			return c.fail("missing parameter 'value'");
		if (v->second.type == JsonValue::NUMBER)
			value._u64[0] = static_cast<u64>(static_cast<int64_t>(v->second.n));
		else if (!parseU128(v->second.s, value))
			return c.fail("'value' must be a hex string of up to 128 bits");

		// Writing the low 64 bits of a GPR shouldn't clobber the upper half.
		if (c.req.boolean("preserve_upper", true) && v->second.type == JsonValue::NUMBER)
			value._u64[1] = c.cpu->getRegister(cat, num)._u64[1];

		DebugInterface* cpu = c.cpu;
		runOnCpuThread([cpu, cat, num, value] { cpu->setRegister(cat, num, value); });

		c.out.key("register").str(c.cpu->getRegisterName(cat, num));
		c.out.key("value").hex128(c.cpu->getRegister(cat, num));
		return true;
	}

	static bool cmdSetPc(Context& c)
	{
		if (!requirePaused(c))
			return false;
		u32 pc;
		if (!getAddress(c, "value", pc))
			return false;
		DebugInterface* cpu = c.cpu;
		runOnCpuThread([cpu, pc] { cpu->setPc(pc); });
		c.out.key("pc").hex32(c.cpu->getPC());
		return true;
	}

	static constexpr int MAX_READ = 1024 * 1024;

	static bool cmdReadMemory(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 addr;
		if (!getAddress(c, "address", addr))
			return false;
		const int len = static_cast<int>(std::clamp<int64_t>(c.req.integer("length", 256), 0, MAX_READ));

		std::string hex;
		hex.reserve(len * 2);
		int invalid = 0;
		static constexpr char digits[] = "0123456789abcdef";
		for (int i = 0; i < len; i++)
		{
			bool valid = true;
			const u8 b = c.cpu->Read8(addr + i, &valid);
			if (!valid)
				invalid++;
			hex += digits[b >> 4];
			hex += digits[b & 15];
		}
		c.out.key("address").hex32(addr);
		c.out.key("length").num(len);
		c.out.key("invalid_bytes").num(invalid);
		c.out.key("hex").str(hex);
		return true;
	}

	static bool cmdWriteMemory(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 addr;
		if (!getAddress(c, "address", addr))
			return false;
		std::string hex = c.req.str("data");
		hex.erase(std::remove_if(hex.begin(), hex.end(), [](char ch) { return ch == ' ' || ch == '\n' || ch == '\t'; }), hex.end());
		if (hex.size() % 2 != 0 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
			return c.fail("'data' must be an even-length hex string");

		std::vector<u8> bytes(hex.size() / 2);
		for (size_t i = 0; i < bytes.size(); i++)
			bytes[i] = static_cast<u8>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));

		DebugInterface* cpu = c.cpu;
		int written = 0;
		runOnCpuThread([cpu, addr, &bytes, &written] {
			for (size_t i = 0; i < bytes.size(); i++)
				written += cpu->Write8(addr + static_cast<u32>(i), bytes[i]) ? 1 : 0;
		});
		c.out.key("address").hex32(addr);
		c.out.key("written").num(written);
		if (written != static_cast<int>(bytes.size()))
			return c.fail("only " + std::to_string(written) + " of " + std::to_string(bytes.size()) + " bytes were writable");
		return true;
	}

	static bool cmdReadString(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 addr;
		if (!getAddress(c, "address", addr))
			return false;
		const int maxLen = static_cast<int>(std::clamp<int64_t>(c.req.integer("max_length", 256), 1, 65536));

		std::string s;
		bool terminated = false;
		for (int i = 0; i < maxLen; i++)
		{
			bool valid = true;
			const u8 b = c.cpu->Read8(addr + i, &valid);
			if (!valid || b == 0)
			{
				terminated = valid;
				break;
			}
			s += static_cast<char>(b);
		}
		// Hex too, since game strings are frequently Shift-JIS rather than ASCII.
		std::string hex;
		static constexpr char digits[] = "0123456789abcdef";
		for (const char ch : s)
		{
			hex += digits[static_cast<u8>(ch) >> 4];
			hex += digits[static_cast<u8>(ch) & 15];
		}
		c.out.key("address").hex32(addr);
		c.out.key("length").unum(s.size());
		c.out.key("terminated").boolean(terminated);
		c.out.key("hex").str(hex);
		return true;
	}

	static bool cmdFindMemory(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 start, end;
		if (!getAddress(c, "start", start, 0x00100000u) || !getAddress(c, "end", end, 0x02000000u))
			return false;
		if (end <= start)
			return c.fail("'end' must be greater than 'start'");
		const int maxResults = static_cast<int>(std::clamp<int64_t>(c.req.integer("max_results", 50), 1, 10000));
		const u32 align = static_cast<u32>(std::clamp<int64_t>(c.req.integer("align", 1), 1, 16));

		// Pattern: hex bytes with optional whitespace, "??" for wildcard bytes.
		std::string pat = c.req.str("pattern");
		pat.erase(std::remove_if(pat.begin(), pat.end(), [](char ch) { return ch == ' ' || ch == '\t'; }), pat.end());
		if (pat.empty() || pat.size() % 2 != 0)
			return c.fail("'pattern' must be pairs of hex digits or ??");
		std::vector<int> needle;
		for (size_t i = 0; i < pat.size(); i += 2)
		{
			const std::string byte = pat.substr(i, 2);
			if (byte == "??")
				needle.push_back(-1);
			else if (byte.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
				needle.push_back(static_cast<int>(std::strtoul(byte.c_str(), nullptr, 16)));
			else
				return c.fail("invalid pattern byte '" + byte + "'");
		}

		// Read the whole range once; invalid bytes are marked so they never match.
		const u32 size = end - start;
		if (size > 64 * 1024 * 1024)
			return c.fail("search range is limited to 64MB");
		std::vector<s16> mem(size);
		for (u32 i = 0; i < size; i++)
		{
			bool valid = true;
			const u8 b = c.cpu->Read8(start + i, &valid);
			mem[i] = valid ? b : -2;
		}

		c.out.key("matches").beginArray();
		int found = 0;
		bool truncated = false;
		for (u32 i = 0; i + needle.size() <= size; i += align)
		{
			bool match = true;
			for (size_t k = 0; k < needle.size(); k++)
			{
				if (mem[i + k] < 0 || (needle[k] >= 0 && mem[i + k] != needle[k]))
				{
					match = false;
					break;
				}
			}
			if (!match)
				continue;
			if (found == maxResults)
			{
				truncated = true;
				break;
			}
			c.out.hex32(start + i);
			found++;
		}
		c.out.endArray();
		c.out.key("truncated").boolean(truncated);
		return true;
	}

	static bool cmdDisassemble(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 addr;
		if (!getAddress(c, "address", addr, c.cpu->getPC()))
			return false;
		addr &= ~3u;
		const int count = static_cast<int>(std::clamp<int64_t>(c.req.integer("count", 20), 1, 1000));
		const int before = static_cast<int>(std::clamp<int64_t>(c.req.integer("before", 0), 0, 500));
		const bool simplify = c.req.boolean("simplify", true);
		const u32 pc = c.cpu->getPC();

		addr -= before * 4;
		c.out.key("instructions").beginArray();
		for (int i = 0; i < count + before; i++)
		{
			const u32 a = addr + i * 4;
			if (!c.cpu->isValidAddress(a))
				break;
			c.out.beginObject();
			c.out.key("address").hex32(a);
			c.out.key("opcode").hex32(c.cpu->Read32(a));
			c.out.key("disasm").str(c.cpu->disasm(a, simplify));
			const std::string label = functionNameAt(c.cpu, a, false);
			if (!label.empty())
				c.out.key("label").str(label);
			if (a == pc)
				c.out.key("is_pc").boolean(true);
			if (CBreakPoints::IsAddressBreakPoint(c.bpCpu, a))
				c.out.key("breakpoint").boolean(true);
			c.out.endObject();
		}
		c.out.endArray();
		return true;
	}

	static bool cmdEvaluate(Context& c)
	{
		if (!requireVM(c))
			return false;
		const std::string expr = c.req.str("expression");
		u64 result = 0;
		std::string error;
		if (!c.cpu->evaluateExpression(expr.c_str(), result, error))
			return c.fail(error.empty() ? "invalid expression" : error);
		char hex[24];
		std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(result));
		c.out.key("value").unum(result);
		c.out.key("hex").str(hex);
		c.out.key("signed").num(static_cast<int64_t>(static_cast<s32>(result)));
		return true;
	}

	static bool parseCondition(Context& c, BreakPointCond& cond, bool& has)
	{
		const std::string expr = c.req.str("condition");
		has = !expr.empty();
		if (!has)
			return true;
		std::string error;
		cond.debug = c.cpu;
		cond.expressionString = expr;
		if (!c.cpu->initExpression(expr.c_str(), cond.expression, error))
			return c.fail("invalid condition '" + expr + "': " + error);
		return true;
	}

	// action: "break" (default) pauses; "log" only counts hits and logs them
	// to the PCSX2 console without pausing; "both" logs and pauses.
	static bool parseAction(Context& c, bool& log, bool& cont, std::string& format)
	{
		const std::string action = c.req.str("action", "break");
		if (action != "break" && action != "log" && action != "both")
			return c.fail("'action' must be break, log or both");
		log = action != "break";
		cont = action == "log";
		format = c.req.str("log_format");
		std::string error;
		if (log && !format.empty() && !ValidateInstrumentationLogFormat(*c.cpu, format, error))
			return c.fail("invalid log_format: " + error);
		return true;
	}

	static bool cmdSetBreakpoint(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 addr;
		if (!getAddress(c, "address", addr))
			return false;
		BreakPointCond cond;
		bool hasCond, log, cont;
		std::string format;
		if (!parseCondition(c, cond, hasCond) || !parseAction(c, log, cont, format))
			return false;
		const bool enabled = c.req.boolean("enabled", true);
		const bool temporary = c.req.boolean("temporary", false);
		const std::string desc = c.req.str("description");
		const u32 maxHits = static_cast<u32>(c.req.integer("max_hits", 0));
		const BreakPointCpu cpu = c.bpCpu;

		runOnCpuThread([=] {
			// Re-adding replaces: drop the old one so stale conditions don't linger.
			if (!temporary)
				CBreakPoints::RemoveBreakPoint(cpu, addr);
			CBreakPoints::AddBreakPoint(cpu, addr, temporary, enabled);
			if (temporary)
				return;
			if (hasCond)
				CBreakPoints::ChangeBreakPointAddCond(cpu, addr, cond);
			if (!desc.empty())
				CBreakPoints::ChangeBreakPointDescription(cpu, addr, desc);
			if (maxHits)
				CBreakPoints::ChangeBreakPointMaxHits(cpu, addr, maxHits);
			if (log)
				CBreakPoints::ChangeBreakPointInstrumentation(cpu, addr, true, format, cont);
		});
		c.out.key("address").hex32(addr);
		const std::string fn = functionNameAt(c.cpu, addr, true);
		if (!fn.empty())
			c.out.key("function").str(fn);
		return true;
	}

	static bool cmdRemoveBreakpoint(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 addr;
		if (!getAddress(c, "address", addr))
			return false;
		if (!CBreakPoints::IsAddressBreakPoint(c.bpCpu, addr, nullptr))
			return c.fail("no breakpoint at that address");
		const BreakPointCpu cpu = c.bpCpu;
		runOnCpuThread([cpu, addr] { CBreakPoints::RemoveBreakPoint(cpu, addr); });
		c.out.key("address").hex32(addr);
		return true;
	}

	static bool cmdEnableBreakpoint(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 addr;
		if (!getAddress(c, "address", addr))
			return false;
		if (!CBreakPoints::IsAddressBreakPoint(c.bpCpu, addr, nullptr))
			return c.fail("no breakpoint at that address");
		const bool enabled = c.req.boolean("enabled", true);
		const BreakPointCpu cpu = c.bpCpu;
		runOnCpuThread([cpu, addr, enabled] { CBreakPoints::ChangeBreakPoint(cpu, addr, enabled); });
		c.out.key("address").hex32(addr);
		c.out.key("enabled").boolean(enabled);
		return true;
	}

	static void writeBreakpoint(JsonWriter& j, const BreakPoint& bp)
	{
		DebugInterface* cpu = &DebugInterface::get(bp.cpu);
		j.beginObject();
		j.key("cpu").str(bp.cpu == BREAKPOINT_IOP ? "iop" : "ee");
		j.key("address").hex32(bp.addr);
		j.key("enabled").boolean(bp.enabled);
		if (bp.temporary)
			j.key("temporary").boolean(true);
		if (bp.hasCond)
			j.key("condition").str(bp.cond.expressionString);
		if (!bp.description.empty())
			j.key("description").str(bp.description);
		j.key("hits").unum(bp.totalHits);
		if (bp.maxHits)
			j.key("max_hits").unum(bp.maxHits);
		if (bp.instrumentationEnabled)
			j.key("action").str(bp.continueOnHit ? "log" : "both");
		const std::string fn = functionNameAt(cpu, bp.addr, true);
		if (!fn.empty())
			j.key("function").str(fn);
		j.endObject();
	}

	static void writeMemcheck(JsonWriter& j, const MemCheck& mc)
	{
		j.beginObject();
		j.key("cpu").str(mc.cpu == BREAKPOINT_IOP ? "iop" : "ee");
		j.key("start").hex32(mc.start);
		j.key("end").hex32(mc.end);
		const char* type = (mc.memCond & MEMCHECK_WRITE_ONCHANGE) ? "onchange" :
			(mc.memCond & MEMCHECK_READWRITE) == MEMCHECK_READWRITE ? "readwrite" :
			(mc.memCond & MEMCHECK_READ) ? "read" : "write";
		j.key("type").str(type);
		j.key("enabled").boolean(mc.result != MEMCHECK_IGNORE);
		if (mc.hasCond)
			j.key("condition").str(mc.cond.expressionString);
		if (!mc.description.empty())
			j.key("description").str(mc.description);
		j.key("hits").unum(mc.totalHits);
		if (mc.lastSize != 0)
		{
			j.key("last_pc").hex32(mc.lastPC);
			j.key("last_access").hex32(mc.lastAddr);
		}
		if (mc.instrumentationEnabled)
			j.key("action").str(mc.continueOnHit ? "log" : "both");
		j.endObject();
	}

	static bool cmdListBreakpoints(Context& c)
	{
		const bool all = !c.req.has("cpu");
		c.out.key("breakpoints").beginArray();
		for (const BreakPointCpu cpu : DEBUG_CPUS)
		{
			if (all || cpu == c.bpCpu)
			{
				for (const BreakPoint& bp : CBreakPoints::GetBreakpoints(cpu, c.req.boolean("include_temporary", false)))
					writeBreakpoint(c.out, bp);
			}
		}
		c.out.endArray();
		c.out.key("watchpoints").beginArray();
		for (const BreakPointCpu cpu : DEBUG_CPUS)
		{
			if (all || cpu == c.bpCpu)
			{
				for (const MemCheck& mc : CBreakPoints::GetMemChecks(cpu))
					writeMemcheck(c.out, mc);
			}
		}
		c.out.endArray();
		return true;
	}

	static bool memcheckRange(Context& c, u32& start, u32& end)
	{
		if (!getAddress(c, "address", start))
			return false;
		if (c.req.has("end"))
			return getAddress(c, "end", end);
		end = start + static_cast<u32>(std::clamp<int64_t>(c.req.integer("size", 4), 1, 0x7fffffff));
		return true;
	}

	static bool cmdSetMemcheck(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 start, end;
		if (!memcheckRange(c, start, end))
			return false;
		if (end <= start)
			return c.fail("'end' must be greater than 'address'");

		const std::string type = c.req.str("type", "write");
		MemCheckCondition memCond;
		if (type == "read")
			memCond = MEMCHECK_READ;
		else if (type == "write")
			memCond = MEMCHECK_WRITE;
		else if (type == "readwrite" || type == "access")
			memCond = MEMCHECK_READWRITE;
		else if (type == "onchange")
			memCond = static_cast<MemCheckCondition>(MEMCHECK_WRITE | MEMCHECK_WRITE_ONCHANGE);
		else
			return c.fail("'type' must be read, write, readwrite or onchange");

		BreakPointCond cond;
		bool hasCond, log, cont;
		std::string format;
		if (!parseCondition(c, cond, hasCond) || !parseAction(c, log, cont, format))
			return false;
		const std::string desc = c.req.str("description");
		const u32 maxHits = static_cast<u32>(c.req.integer("max_hits", 0));
		const BreakPointCpu cpu = c.bpCpu;

		runOnCpuThread([=] {
			CBreakPoints::RemoveMemCheck(cpu, start, end);
			CBreakPoints::AddMemCheck(cpu, start, end, memCond, MEMCHECK_BREAK);
			if (hasCond)
				CBreakPoints::ChangeMemCheckAddCond(cpu, start, end, cond);
			if (!desc.empty())
				CBreakPoints::ChangeMemCheckDescription(cpu, start, end, desc);
			if (maxHits)
				CBreakPoints::ChangeMemCheckMaxHits(cpu, start, end, maxHits);
			if (log)
				CBreakPoints::ChangeMemCheckInstrumentation(cpu, start, end, true, format, cont);
		});
		c.out.key("start").hex32(start);
		c.out.key("end").hex32(end);
		return true;
	}

	static bool findMemcheck(Context& c, u32& start, u32& end)
	{
		if (!memcheckRange(c, start, end))
			return false;
		for (const MemCheck& mc : CBreakPoints::GetMemChecks(c.bpCpu))
		{
			// Allow identifying a watchpoint by its start address only.
			if (mc.start == start && (mc.end == end || !c.req.has("end") && !c.req.has("size")))
			{
				end = mc.end;
				return true;
			}
		}
		return c.fail("no watchpoint at that address");
	}

	static bool cmdRemoveMemcheck(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 start, end;
		if (!findMemcheck(c, start, end))
			return false;
		const BreakPointCpu cpu = c.bpCpu;
		runOnCpuThread([cpu, start, end] { CBreakPoints::RemoveMemCheck(cpu, start, end); });
		c.out.key("start").hex32(start);
		c.out.key("end").hex32(end);
		return true;
	}

	static bool cmdEnableMemcheck(Context& c)
	{
		if (!requireVM(c))
			return false;
		u32 start, end;
		if (!findMemcheck(c, start, end))
			return false;
		MemCheckCondition memCond = MEMCHECK_WRITE;
		for (const MemCheck& mc : CBreakPoints::GetMemChecks(c.bpCpu))
		{
			if (mc.start == start && mc.end == end)
				memCond = mc.memCond;
		}
		const bool enabled = c.req.boolean("enabled", true);
		const BreakPointCpu cpu = c.bpCpu;
		runOnCpuThread([=] { CBreakPoints::ChangeMemCheck(cpu, start, end, memCond, enabled ? MEMCHECK_BREAK : MEMCHECK_IGNORE); });
		c.out.key("enabled").boolean(enabled);
		return true;
	}

	static bool cmdClearBreakpoints(Context& c)
	{
		runOnCpuThread([] {
			CBreakPoints::ClearAllBreakPoints();
			CBreakPoints::ClearAllMemChecks();
		});
		return true;
	}

	static bool cmdPause(Context& c)
	{
		if (!requireVM(c))
			return false;
		runOnCpuThread([] { r5900Debug.pauseCpu(); });
		waitForPause(2000);
		runOnCpuThread(cleanupAfterStop);
		writeStop(c.out, "paused");
		return true;
	}

	static bool cmdResume(Context& c)
	{
		if (!requireVM(c))
			return false;
		const bool wasPaused = c.cpu->isCpuPaused();
		if (wasPaused)
			runOnCpuThread(resumeVM);
		c.out.key("was_running").boolean(!wasPaused);
		return true;
	}

	// Blocks until the VM pauses (breakpoint/watchpoint, frame advance, user
	// pause) or the timeout expires.
	static bool cmdWait(Context& c)
	{
		if (!requireVM(c))
			return false;
		const int timeout = static_cast<int>(std::clamp<int64_t>(c.req.integer("timeout_ms", 10000), 0, 600000));
		const bool stopped = waitForPause(timeout);
		if (stopped)
			runOnCpuThread(cleanupAfterStop);
		c.out.key("timed_out").boolean(!stopped);
		writeStop(c.out, "paused");
		return true;
	}

	static bool cmdStep(Context& c)
	{
		if (!requirePaused(c))
			return false;
		const u32 pc = c.cpu->getPC();
		const MIPSAnalyst::MipsOpcodeInfo info = MIPSAnalyst::GetOpcodeInfo(c.cpu, pc);

		// Same target selection as DebuggerWindow::onStepInto.
		u32 target = pc + 4;
		if (info.isBranch)
			target = (!info.isConditional || info.conditionMet) ? info.branchTarget : pc + 8;
		if (info.isSyscall)
			target = info.branchTarget;
		return runUntil(c, target, "step");
	}

	static bool cmdStepOver(Context& c)
	{
		if (!requirePaused(c))
			return false;
		const u32 pc = c.cpu->getPC();
		const MIPSAnalyst::MipsOpcodeInfo info = MIPSAnalyst::GetOpcodeInfo(c.cpu, pc);

		// Same target selection as DebuggerWindow::onStepOver.
		u32 target = pc + 4;
		if (info.isBranch)
		{
			if (!info.isConditional)
				target = info.isLinkedBranch ? pc + 8 : info.branchTarget;
			else
				target = info.conditionMet ? info.branchTarget : pc + 8;
		}
		return runUntil(c, target, "step");
	}

	static std::vector<MipsStackWalk::StackFrame> currentStack(DebugInterface* cpu)
	{
		for (const auto& thread : cpu->GetThreadList())
		{
			if (thread->Status() == ThreadStatus::THS_RUN)
				return cpu->StackTrace(thread.get());
		}
		return MipsStackWalk::Walk(cpu, cpu->getPC(), cpu->getRegister(EECAT_GPR, 31)._u32[0],
			cpu->getRegister(EECAT_GPR, 29)._u32[0], 0);
	}

	static bool cmdStepOut(Context& c)
	{
		if (!requirePaused(c))
			return false;
		const auto frames = currentStack(c.cpu);
		if (frames.size() < 2)
			return c.fail("could not determine the caller (stack walk found no parent frame)");
		return runUntil(c, frames[1].pc, "step");
	}

	static bool cmdRunTo(Context& c)
	{
		if (!requirePaused(c))
			return false;
		u32 target;
		if (!getAddress(c, "address", target))
			return false;
		return runUntil(c, target, "run_to");
	}

	// Runs `frames` frames from a paused state and pauses again.
	static void advanceFrames(Context& c, u32 frames)
	{
		runOnCpuThread([frames] {
			{
				std::lock_guard lock(s_hitMutex);
				s_hitSnapshot = collectHits();
			}
			CBreakPoints::SetSkipFirst(BREAKPOINT_EE, r5900Debug.getPC());
			CBreakPoints::SetSkipFirst(BREAKPOINT_IOP, r3000Debug.getPC());
			VMManager::FrameAdvance(frames);
		});
		const int timeout = static_cast<int>(std::clamp<int64_t>(c.req.integer("timeout_ms", 30000), 1, 600000));
		const bool stopped = waitForPause(timeout);
		if (stopped)
			runOnCpuThread(cleanupAfterStop);
		c.out.key("timed_out").boolean(!stopped);
		writeStop(c.out, "frame_advance");
	}

	static bool cmdFrameAdvance(Context& c)
	{
		if (!requirePaused(c))
			return false;
		advanceFrames(c, static_cast<u32>(std::clamp<int64_t>(c.req.integer("frames", 1), 1, 600)));
		return true;
	}

	// ============================================================
	// Controller input (DualShock 2). Inputs are set the same way bound host
	// buttons are, so they persist until changed — by us or by a real
	// controller event for the same input.
	// ============================================================
	static constexpr const char* s_padInputNames[] = {
		"up", "right", "down", "left", "triangle", "circle", "cross", "square",
		"select", "start", "l1", "l2", "r1", "r2", "l3", "r3", "analog", "pressure",
		"l_up", "l_right", "l_down", "l_left", "r_up", "r_right", "r_down", "r_left",
	};
	static_assert(std::size(s_padInputNames) == PadDualshock2::Inputs::LENGTH);

	static bool getPadPort(Context& c, u32& port)
	{
		if (!requireVM(c))
			return false;
		const int64_t p = c.req.integer("port", 1);
		if (p < 1 || p > static_cast<int64_t>(Pad::NUM_CONTROLLER_PORTS))
			return c.fail("'port' must be between 1 and " + std::to_string(Pad::NUM_CONTROLLER_PORTS));
		port = static_cast<u32>(p - 1);
		const PadBase* pad = Pad::GetPad(static_cast<u8>(port));
		if (!pad || pad->GetType() != Pad::ControllerType::DualShock2)
			return c.fail("port " + std::to_string(p) + " has no DualShock 2 controller configured");
		return true;
	}

	// Parses "cross,start,l_left=0.5" into (input, value) pairs; values default
	// to 1 (fully pressed) and range from 0 to 1.
	static bool parsePadInputs(Context& c, std::vector<std::pair<u32, float>>& out, bool required)
	{
		const std::string spec = c.req.str("inputs");
		size_t pos = 0;
		while (pos < spec.size())
		{
			size_t comma = spec.find(',', pos);
			if (comma == std::string::npos)
				comma = spec.size();
			std::string item = spec.substr(pos, comma - pos);
			pos = comma + 1;
			item.erase(std::remove_if(item.begin(), item.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }), item.end());
			if (item.empty())
				continue;

			float value = 1.0f;
			if (const size_t eq = item.find('='); eq != std::string::npos)
			{
				char* end;
				value = std::strtof(item.c_str() + eq + 1, &end);
				if (*end || value < 0.0f || value > 1.0f)
					return c.fail("input value in '" + item + "' must be between 0 and 1");
				item.resize(eq);
			}

			u32 index = PadDualshock2::Inputs::LENGTH;
			for (u32 i = 0; i < PadDualshock2::Inputs::LENGTH; i++)
			{
				if (equalsNoCase(s_padInputNames[i], item.c_str()))
					index = i;
			}
			if (index == PadDualshock2::Inputs::LENGTH)
				return c.fail("unknown input '" + item + "'");
			out.emplace_back(index, value);
		}
		if (required && out.empty())
			return c.fail("'inputs' is empty");
		return true;
	}

	static void setPadInputs(u32 port, const std::vector<std::pair<u32, float>>& inputs, bool release)
	{
		runOnCpuThread([port, inputs, release] {
			for (const auto& [index, value] : inputs)
				Pad::SetControllerState(port, index, release ? 0.0f : value);
		});
	}

	static bool cmdPadSet(Context& c)
	{
		u32 port;
		std::vector<std::pair<u32, float>> inputs;
		if (!getPadPort(c, port) || !parsePadInputs(c, inputs, true))
			return false;
		setPadInputs(port, inputs, false);
		return true;
	}

	static bool cmdPadRelease(Context& c)
	{
		u32 port;
		std::vector<std::pair<u32, float>> inputs;
		if (!getPadPort(c, port) || !parsePadInputs(c, inputs, false))
			return false;
		if (inputs.empty())
		{
			for (u32 i = 0; i < PadDualshock2::Inputs::LENGTH; i++)
				inputs.emplace_back(i, 0.0f);
		}
		setPadInputs(port, inputs, true);
		return true;
	}

	// Holds the inputs for `frames` frames, then releases them. While paused
	// this frame-advances (and stays paused), which makes input deterministic;
	// while running it waits for that many vsyncs.
	static bool cmdPadPress(Context& c)
	{
		u32 port;
		std::vector<std::pair<u32, float>> inputs;
		if (!getPadPort(c, port) || !parsePadInputs(c, inputs, true))
			return false;
		const u32 frames = static_cast<u32>(std::clamp<int64_t>(c.req.integer("frames", 4), 1, 3600));

		setPadInputs(port, inputs, false);
		if (c.cpu->isCpuPaused())
		{
			advanceFrames(c, frames);
		}
		else
		{
			const uint start = g_FrameCount;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10) + std::chrono::milliseconds(frames * 50);
			while (g_FrameCount - start < frames && VMManager::HasValidVM() && !c.cpu->isCpuPaused() &&
				   std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			c.out.key("frames_elapsed").unum(g_FrameCount - start);
		}
		setPadInputs(port, inputs, true);
		return true;
	}

	static const char* threadStatusName(ThreadStatus s)
	{
		switch (s)
		{
			case ThreadStatus::THS_RUN: return "running";
			case ThreadStatus::THS_READY: return "ready";
			case ThreadStatus::THS_WAIT: return "waiting";
			case ThreadStatus::THS_SUSPEND: return "suspended";
			case ThreadStatus::THS_WAIT_SUSPEND: return "waiting+suspended";
			case ThreadStatus::THS_DORMANT: return "dormant";
			default: return "bad";
		}
	}

	static const char* waitStateName(WaitState w)
	{
		switch (w)
		{
			case WaitState::NONE: return "none";
			case WaitState::SEMA: return "semaphore";
			case WaitState::SLEEP: return "sleep";
			case WaitState::DELAY: return "delay";
			case WaitState::EVENTFLAG: return "eventflag";
			case WaitState::MBOX: return "mailbox";
			case WaitState::VPOOL: return "vpool";
			case WaitState::FIXPOOL: return "fpool";
			default: return "unknown";
		}
	}

	static bool cmdGetThreads(Context& c)
	{
		if (!requireVM(c))
			return false;
		c.out.key("threads").beginArray();
		for (const auto& t : c.cpu->GetThreadList())
		{
			c.out.beginObject();
			c.out.key("id").unum(t->TID());
			c.out.key("status").str(threadStatusName(t->Status()));
			c.out.key("wait").str(waitStateName(t->Wait()));
			if (t->Wait() != WaitState::NONE)
				c.out.key("wait_id").unum(t->WaitId());
			c.out.key("priority").unum(t->Priority());
			c.out.key("entry").hex32(t->EntryPoint());
			const std::string fn = functionNameAt(c.cpu, t->EntryPoint(), false);
			if (!fn.empty())
				c.out.key("entry_function").str(fn);
			c.out.key("pc").hex32(t->Status() == ThreadStatus::THS_RUN ? c.cpu->getPC() : t->PC());
			c.out.endObject();
		}
		c.out.endArray();
		return true;
	}

	static bool cmdGetModules(Context& c)
	{
		if (!requireVM(c))
			return false;
		c.out.key("modules").beginArray();
		for (const IopMod& m : r3000Debug.GetModuleList())
		{
			c.out.beginObject();
			c.out.key("name").str(m.name);
			char ver[16];
			std::snprintf(ver, sizeof(ver), "%d.%d", m.version >> 8, m.version & 0xff);
			c.out.key("version").str(ver);
			c.out.key("text_addr").hex32(m.text_addr);
			c.out.key("entry").hex32(m.entry);
			c.out.key("gp").hex32(m.gp);
			c.out.key("text_size").unum(m.text_size);
			c.out.key("data_size").unum(m.data_size);
			c.out.key("bss_size").unum(m.bss_size);
			c.out.endObject();
		}
		c.out.endArray();
		return true;
	}

	static bool cmdGetBacktrace(Context& c)
	{
		if (!requireVM(c))
			return false;
		const int maxFrames = static_cast<int>(std::clamp<int64_t>(c.req.integer("max_frames", 32), 1, 256));

		std::vector<MipsStackWalk::StackFrame> frames;
		if (c.req.has("thread"))
		{
			const u32 tid = static_cast<u32>(c.req.integer("thread"));
			bool found = false;
			for (const auto& t : c.cpu->GetThreadList())
			{
				if (t->TID() == tid)
				{
					frames = c.cpu->StackTrace(t.get());
					found = true;
					break;
				}
			}
			if (!found)
				return c.fail("no thread with id " + std::to_string(tid));
		}
		else
		{
			frames = currentStack(c.cpu);
		}

		c.out.key("frames").beginArray();
		for (int i = 0; i < static_cast<int>(frames.size()) && i < maxFrames; i++)
		{
			const auto& f = frames[i];
			// The walk ends with a sentinel frame once it runs past the thread entry.
			if (!c.cpu->isValidAddress(f.pc))
				break;
			c.out.beginObject();
			c.out.key("entry").hex32(f.entry);
			const std::string fn = functionNameAt(c.cpu, f.entry, false);
			if (!fn.empty())
				c.out.key("entry_function").str(fn);
			writeLocation(c.out, c.cpu, f.pc);
			c.out.key("sp").hex32(f.sp);
			c.out.key("stack_size").num(f.stackSize);
			c.out.endObject();
		}
		c.out.endArray();
		return true;
	}

	static const char* symbolKindName(ccc::SymbolDescriptor d)
	{
		switch (d)
		{
			case ccc::DATA_TYPE: return "data_type";
			case ccc::FUNCTION: return "function";
			case ccc::GLOBAL_VARIABLE: return "global_variable";
			case ccc::LABEL: return "label";
			case ccc::LOCAL_VARIABLE: return "local_variable";
			case ccc::MODULE: return "module";
			case ccc::PARAMETER_VARIABLE: return "parameter_variable";
			case ccc::SECTION: return "section";
			case ccc::SOURCE_FILE: return "source_file";
			case ccc::SYMBOL_SOURCE: return "symbol_source";
		}
		return "unknown";
	}

	// (Re)runs PCSX2's symbol import and function scan for the current ELF
	// using the analysis options from the debugger settings. Runs in the
	// background; results show up in the symbol database as they're found.
	static bool cmdAnalyze(Context& c)
	{
		if (!requireVM(c))
			return false;
		runOnCpuThread([] { R5900SymbolImporter.LoadAndAnalyseElf(EmuConfig.DebuggerAnalysis); });
		return true;
	}

	static bool cmdLookupSymbol(Context& c)
	{
		if (!requireVM(c))
			return false;
		SymbolGuardian& guardian = c.cpu->GetSymbolGuardian();
		SymbolInfo sym;
		if (c.req.has("name"))
		{
			sym = guardian.SymbolWithName(c.req.str("name"));
		}
		else
		{
			u32 addr;
			if (!getAddress(c, "address", addr))
				return false;
			sym = guardian.SymbolOverlappingAddress(addr);
			if (sym.address.valid())
				c.out.key("offset").unum(addr - sym.address.value);
		}
		if (!sym.address.valid() && sym.name.empty())
			return c.fail("no matching symbol");
		c.out.key("name").str(sym.name);
		if (sym.address.valid())
			c.out.key("address").hex32(sym.address.value);
		c.out.key("size").unum(sym.size);
		if (sym.descriptor)
			c.out.key("kind").str(symbolKindName(*sym.descriptor));
		return true;
	}

	static bool cmdListFunctions(Context& c)
	{
		if (!requireVM(c))
			return false;
		std::string filter = c.req.str("filter");
		std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
		const int limit = static_cast<int>(std::clamp<int64_t>(c.req.integer("limit", 100), 1, 100000));
		u32 lo = 0, hi = 0xffffffff;
		if (c.req.has("start") && !getAddress(c, "start", lo))
			return false;
		if (c.req.has("end") && !getAddress(c, "end", hi))
			return false;

		struct Fn
		{
			std::string name;
			u32 address;
			u32 size;
		};
		std::vector<Fn> fns;
		size_t total = 0;
		c.cpu->GetSymbolGuardian().Read([&](const ccc::SymbolDatabase& db) {
			for (const ccc::Function& f : db.functions)
			{
				if (!f.address().valid() || f.address().value < lo || f.address().value >= hi)
					continue;
				if (!filter.empty())
				{
					std::string lower = f.name();
					std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
					if (lower.find(filter) == std::string::npos)
						continue;
				}
				total++;
				if (static_cast<int>(fns.size()) < limit)
					fns.push_back({f.name(), f.address().value, f.size()});
			}
		});
		std::sort(fns.begin(), fns.end(), [](const Fn& a, const Fn& b) { return a.address < b.address; });
		c.out.key("total").unum(total);
		c.out.key("functions").beginArray();
		for (const Fn& f : fns)
		{
			c.out.beginObject();
			c.out.key("name").str(f.name);
			c.out.key("address").hex32(f.address);
			c.out.key("size").unum(f.size);
			c.out.endObject();
		}
		c.out.endArray();
		return true;
	}

	static std::string base64(const std::vector<u8>& data)
	{
		static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		out.reserve((data.size() + 2) / 3 * 4);
		size_t i = 0;
		for (; i + 2 < data.size(); i += 3)
		{
			const u32 v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
			out += tbl[v >> 18];
			out += tbl[(v >> 12) & 63];
			out += tbl[(v >> 6) & 63];
			out += tbl[v & 63];
		}
		if (i < data.size())
		{
			const u32 v = (data[i] << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0);
			out += tbl[v >> 18];
			out += tbl[(v >> 12) & 63];
			out += i + 1 < data.size() ? tbl[(v >> 6) & 63] : '=';
			out += '=';
		}
		return out;
	}

	static bool cmdScreenshot(Context& c)
	{
		if (!requireVM(c))
			return false;
		const u32 width = static_cast<u32>(std::clamp<int64_t>(c.req.integer("width", 0), 0, 4096));
		const u32 height = static_cast<u32>(std::clamp<int64_t>(c.req.integer("height", 0), 0, 4096));

		u32 w = 0, h = 0;
		std::vector<u32> pixels;
		bool ok = false;
		runOnCpuThread([&] { ok = MTGS::SaveMemorySnapshot(width, height, true, true, &w, &h, &pixels); });
		if (!ok || pixels.empty())
			return c.fail("the GS has no frame to capture");
		for (u32& p : pixels)
			p |= 0xFF000000u; // the display output's alpha is meaningless
		const RGBA8Image image(w, h, std::move(pixels));
		const auto png = image.SaveToBuffer("screenshot.png");
		if (!png)
			return c.fail("PNG encoding failed");
		c.out.key("width").unum(w);
		c.out.key("height").unum(h);
		c.out.key("png_base64").str(base64(*png));
		return true;
	}

	static bool cmdSaveState(Context& c)
	{
		if (!requireVM(c))
			return false;
		const std::string path = c.req.str("path");
		const s32 slot = static_cast<s32>(c.req.integer("slot", -1));
		if (path.empty() && (slot < 1 || slot > 10))
			return c.fail("pass 'path' or a 'slot' between 1 and 10");

		std::string error;
		runOnCpuThread([&] {
			auto onError = [&error](const std::string& msg) { error = msg; };
			if (!path.empty())
				VMManager::SaveState(path.c_str(), false, false, onError);
			else
				VMManager::SaveStateToSlot(slot, false, onError);
		});
		if (!error.empty())
			return c.fail(error);
		if (!path.empty())
			c.out.key("path").str(path);
		else
			c.out.key("slot").num(slot);
		return true;
	}

	static bool cmdLoadState(Context& c)
	{
		if (!requireVM(c))
			return false;
		const std::string path = c.req.str("path");
		const s32 slot = static_cast<s32>(c.req.integer("slot", -1));
		if (path.empty() && (slot < 1 || slot > 10))
			return c.fail("pass 'path' or a 'slot' between 1 and 10");

		bool ok = false;
		Error error;
		runOnCpuThread([&] {
			ok = !path.empty() ? VMManager::LoadState(path.c_str(), &error) : VMManager::LoadStateFromSlot(slot, false, &error);
			if (ok)
				cleanupAfterStop();
		});
		if (!ok)
			return c.fail(error.GetDescription().empty() ? "failed to load state" : error.GetDescription());
		c.out.key("paused").boolean(c.cpu->isCpuPaused());
		c.out.key("pc").hex32(r5900Debug.getPC());
		return true;
	}

	struct Command
	{
		const char* name;
		Handler handler;
		bool exclusive; // serialize against other exclusive commands
	};

	static constexpr Command s_commands[] = {
		{"ping", cmdPing, false},
		{"status", cmdStatus, false},
		{"read_registers", cmdReadRegisters, false},
		{"write_register", cmdWriteRegister, true},
		{"set_pc", cmdSetPc, true},
		{"read_memory", cmdReadMemory, false},
		{"write_memory", cmdWriteMemory, true},
		{"read_string", cmdReadString, false},
		{"find_memory", cmdFindMemory, false},
		{"disassemble", cmdDisassemble, false},
		{"evaluate", cmdEvaluate, false},
		{"analyze", cmdAnalyze, true},
		{"lookup_symbol", cmdLookupSymbol, false},
		{"list_functions", cmdListFunctions, false},
		{"set_breakpoint", cmdSetBreakpoint, true},
		{"remove_breakpoint", cmdRemoveBreakpoint, true},
		{"enable_breakpoint", cmdEnableBreakpoint, true},
		{"list_breakpoints", cmdListBreakpoints, false},
		{"set_memcheck", cmdSetMemcheck, true},
		{"remove_memcheck", cmdRemoveMemcheck, true},
		{"enable_memcheck", cmdEnableMemcheck, true},
		{"clear_breakpoints", cmdClearBreakpoints, true},
		{"pause", cmdPause, false},
		{"resume", cmdResume, true},
		{"wait", cmdWait, false},
		{"step", cmdStep, true},
		{"step_over", cmdStepOver, true},
		{"step_out", cmdStepOut, true},
		{"run_to", cmdRunTo, true},
		{"frame_advance", cmdFrameAdvance, true},
		{"pad_set", cmdPadSet, true},
		{"pad_release", cmdPadRelease, true},
		{"pad_press", cmdPadPress, true},
		{"get_threads", cmdGetThreads, false},
		{"get_modules", cmdGetModules, false},
		{"get_backtrace", cmdGetBacktrace, false},
		{"screenshot", cmdScreenshot, false},
		{"save_state", cmdSaveState, true},
		{"load_state", cmdLoadState, true},
	};

	static std::mutex s_exclusiveMutex;

	static std::string handleLine(std::string_view line)
	{
		Request req;
		std::string error;
		std::string id;
		std::string payload;
		bool ok = false;

		if (JsonParser(line).parseObject(req.params, error))
		{
			id = req.str("id");
			const std::string cmd = req.str("cmd");
			const std::string cpuName = req.str("cpu", "ee");
			const bool iop = cpuName == "iop" || cpuName == "IOP" || cpuName == "r3000";
			DebugInterface* cpu = iop ? static_cast<DebugInterface*>(&r3000Debug) : &r5900Debug;

			const Command* command = nullptr;
			for (const Command& c : s_commands)
			{
				if (cmd == c.name)
					command = &c;
			}

			if (!command)
			{
				error = "unknown command '" + cmd + "'";
			}
			else
			{
				JsonWriter out;
				out.beginObject();
				Context ctx{req, cpu, iop ? BREAKPOINT_IOP : BREAKPOINT_EE, out, error};
				if (command->exclusive)
				{
					std::lock_guard lock(s_exclusiveMutex);
					ok = command->handler(ctx);
				}
				else
				{
					ok = command->handler(ctx);
				}
				out.endObject();
				// Strip the braces; the fields are spliced into the response object.
				payload = out.result().substr(1, out.result().size() - 2);
			}
		}

		JsonWriter j;
		j.beginObject();
		j.key("ok").boolean(ok);
		if (!id.empty())
			j.key("id").str(id);
		if (ok && !payload.empty())
			j.raw(payload);
		if (!ok)
			j.key("error").str(error.empty() ? "command failed" : error);
		j.endObject();
		return j.result();
	}

	// ============================================================
	// TCP server
	// ============================================================
	static std::atomic<bool> s_running{false};
	static socket_t s_listenSocket = SOCKET_INVALID;
	static std::mutex s_clientsMutex;
	static std::set<socket_t> s_clients;

	static bool sendAll(socket_t sock, const std::string& data)
	{
		size_t sent = 0;
		while (sent < data.size())
		{
			const int n = send(sock, data.data() + sent, static_cast<int>(std::min<size_t>(data.size() - sent, 1 << 20)), SEND_FLAGS);
			if (n <= 0)
				return false;
			sent += static_cast<size_t>(n);
		}
		return true;
	}

	static void clientHandler(socket_t sock)
	{
		std::string buffer;
		char chunk[8192];

		while (s_running.load())
		{
			const int n = recv(sock, chunk, sizeof(chunk), 0);
			if (n <= 0)
				break;
			buffer.append(chunk, n);
			if (buffer.size() > 16 * 1024 * 1024)
				break; // runaway client

			size_t start = 0, nl;
			bool alive = true;
			while (alive && (nl = buffer.find('\n', start)) != std::string::npos)
			{
				std::string_view line(buffer.data() + start, nl - start);
				start = nl + 1;
				while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
					line.remove_suffix(1);
				if (line.empty())
					continue;
				alive = sendAll(sock, handleLine(line) + '\n');
			}
			buffer.erase(0, start);
			if (!alive)
				break;
		}

		{
			std::lock_guard lock(s_clientsMutex);
			s_clients.erase(sock);
		}
		CLOSE_SOCKET(sock);
		std::fprintf(stderr, "[DebugServer] Client disconnected\n");
	}

	static void serverLoop(int port)
	{
		s_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s_listenSocket == SOCKET_INVALID)
		{
			std::fprintf(stderr, "[DebugServer] Failed to create socket\n");
			s_running.store(false);
			return;
		}

		int opt = 1;
		setsockopt(s_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
		addr.sin_port = htons(static_cast<u_short>(port));

		if (bind(s_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s_listenSocket, 4) != 0)
		{
			std::fprintf(stderr, "[DebugServer] Failed to listen on 127.0.0.1:%d\n", port);
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
			s_running.store(false);
			return;
		}

		std::fprintf(stderr, "[DebugServer] Listening on 127.0.0.1:%d\n", port);

		while (s_running.load())
		{
			// select() with a timeout so Stop() is noticed promptly.
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(s_listenSocket, &readSet);
			timeval tv = {0, 250000};
			if (select(static_cast<int>(s_listenSocket) + 1, &readSet, nullptr, nullptr, &tv) <= 0)
				continue;

			const socket_t client = accept(s_listenSocket, nullptr, nullptr);
			if (client == SOCKET_INVALID)
				continue;
			setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&opt), sizeof(opt));

			std::fprintf(stderr, "[DebugServer] Client connected\n");
			// Behave like the Qt debugger being opened: import ELF symbols and
			// scan for functions (subject to the debugger analysis settings).
			Host::RunOnCPUThread([] { R5900SymbolImporter.OnDebuggerOpened(); });
			{
				std::lock_guard lock(s_clientsMutex);
				s_clients.insert(client);
			}
			std::thread(clientHandler, client).detach();
		}

		CLOSE_SOCKET(s_listenSocket);
		s_listenSocket = SOCKET_INVALID;
	}

	static std::thread s_serverThread;

	void Start(int port)
	{
		if (s_running.exchange(true))
			return;
#ifdef _WIN32
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
		if (const char* env = std::getenv("PCSX2_DEBUG_SERVER_PORT"))
			port = std::atoi(env);
		s_serverThread = std::thread(serverLoop, port);
	}

	void Stop()
	{
		if (!s_running.exchange(false))
			return;
		if (s_serverThread.joinable())
			s_serverThread.join();
		// Wake up client threads blocked in recv(); they close their own sockets.
		std::lock_guard lock(s_clientsMutex);
		for (const socket_t sock : s_clients)
		{
#ifdef _WIN32
			shutdown(sock, SD_BOTH);
#else
			shutdown(sock, SHUT_RDWR);
#endif
		}
	}

	bool IsRunning()
	{
		return s_running.load();
	}

} // namespace DebugServer
