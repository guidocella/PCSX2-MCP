// SPDX-FileCopyrightText: 2026 PS2Recomp Debug Bridge
// SPDX-License-Identifier: GPL-3.0+
//
// PCSX2 Debug Server — a JSON/TCP debugging API on top of DebugInterface and
// CBreakPoints, used by pcsx2-mcp-server. See DebugServer.cpp for the protocol.

#pragma once

namespace DebugServer
{
	// Start the debug server on 127.0.0.1:port (the PCSX2_DEBUG_SERVER_PORT
	// environment variable overrides the port). Call from the CPU thread
	// during initialization, e.g. VMManager::Internal::CPUThreadInitialize().
	void Start(int port = 21512);

	// Stop accepting connections and disconnect clients.
	void Stop();

	bool IsRunning();
} // namespace DebugServer
