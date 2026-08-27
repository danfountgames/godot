/**************************************************************************/
/*  mcp_service.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef MCP_SERVICE_H
#define MCP_SERVICE_H

#include "mcp_protocol.h"
#include "mcp_profiler_recorder.h"
#include "mcp_runtime_bridge.h"

#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "editor/plugins/editor_plugin.h"

class MCPApprovalsDialog;
class MCPActivityDock;
class MCPRuntimeBridge;
#ifdef MCP_TERMINAL_ENABLED
class MCPAgentTerminalPanel;
#endif

// Editor-side half of the bridge: a loopback listener that godot-ai-relay connects
// to, following the lifecycle pattern of the in-tree debug adapter server.
//
// The editor never owns stdio for the protocol - the relay does - so engine prints
// can never corrupt a client's stream.
class MCPService : public EditorPlugin, public MCPProtocol::Delegate {
	GDCLASS(MCPService, EditorPlugin);

	struct DeferredCall {
		Variant id;
		int64_t token = 0;
		Ref<MCPTool> tool;
		String checkpoint;
	};

	// A request this editor sent *to* a client - the reverse of everything else here.
	// MCP calls it sampling: the editor has no model, so it borrows the client's.

	struct Peer {
		Ref<StreamPeerTCP> connection;
		String buffer;
		MCPSession session;
		String address;
		// Requests this client is still waiting on. Dropped if it disconnects, so a
		// dialog nobody is listening to cannot answer into a dead socket.
		Vector<DeferredCall> deferred;

		// HTTP peers speak Streamable HTTP MCP straight to the editor - the transport
		// that makes a relay process unnecessary (DEC-0014). Same session and deferred
		// machinery, different framing: one request in, one response out.
		bool http = false;
		// The session id the in-flight request named, so its session can be stored
		// back after dispatch and after a deferred completion.
		String http_session_id;
		// An HTTP request whose tool deferred: the response is owed and nothing else
		// may be parsed from this connection until it is written.
		bool http_awaiting_deferred = false;
	};

	Ref<TCPServer> server;
	// Streamable HTTP listener. A separate socket because the two framings share
	// nothing on the wire; everything behind the socket is shared.
	Ref<TCPServer> http_server;
	Vector<Peer *> peers;
	// MCP sessions by Mcp-Session-Id. A client may carry one session over many HTTP
	// connections, so the session cannot live on the connection the way bridge
	// sessions do.
	HashMap<String, MCPSession> http_sessions;

	int port = 0;
	int configured_port = 6010;
	int http_port = 0;
	// A per-run bearer token: browsers can POST to localhost from any web page, which
	// the raw TCP bridge never had to care about. GODOT_AI_HTTP_TOKEN overrides it so
	// automation can know it in advance.
	String http_token;
	bool started = false;
	bool polling = false;

	String instance_descriptor_path;
	// Clients seen but not yet approved, surfaced to the user for a decision.
	Vector<String> pending_clients;

	MCPApprovalsDialog *approvals_dialog = nullptr;
	MCPActivityDock *activity_dock = nullptr;
#ifdef MCP_TERMINAL_ENABLED
	MCPAgentTerminalPanel *agent_terminal = nullptr;
#endif
	Ref<MCPRuntimeBridge> runtime_bridge;
	Ref<MCPProfilerRecorder> profiler_recorder;


	void _register_editor_commands();
	void _show_approvals();
	void _show_status();

	void _notification(int p_what);

	void _accept_new_peers();
	void _poll_http_peer(Peer *p_peer);
	void _handle_http_request(Peer *p_peer, const struct MCPHttpRequest &p_request);
	void _send_http_response(Peer *p_peer, const Dictionary &p_message);
	void _poll_peer(Peer *p_peer);
	void _drop_peer(int p_index);
	void _send(Peer *p_peer, const Dictionary &p_message);
	void _handle_line(Peer *p_peer, const String &p_line);

	void _write_instance_descriptor();
	void _remove_instance_descriptor();

	void _on_tools_changed();
	void _poll_deferred();
	// Routes a client's answer to a request the editor sent. Returns true when the
	// frame was one of ours and must not go to the protocol handler.

	// The peer whose message is being handled, so defer_response() knows who to hold
	// the request for. Only valid inside _handle_line.
	Peer *current_peer = nullptr;

public:
	MCPService();
	~MCPService();

	// The editor has one service. The agent terminal panel needs to know whether it is
	// listening before telling a user their agent will have no tools.
	static MCPService *get_singleton();

	void start();
	void stop();
	void restart();
	bool is_running() const { return started; }
	int get_port() const { return port; }
	int get_client_count() const { return peers.size(); }

	// Clients awaiting the user's approval, for the settings UI and command palette.
	Vector<String> get_pending_clients() const { return pending_clients; }
	void approve_client_name(const String &p_client_name);
	void revoke_client_name(const String &p_client_name);
	static bool is_client_approved(const String &p_client_name);


	// MCPProtocol::Delegate.
	virtual bool approve_client(MCPSession &p_session, String &r_reason) override;
	virtual bool prompt_for_tool(const MCPSession &p_session, const Ref<MCPTool> &p_tool, const Dictionary &p_arguments, String &r_reason) override;
	virtual void record_invocation(const MCPSession &p_session, const String &p_tool_name, const String &p_summary, bool p_allowed, const String &p_reason) override;
	virtual bool defer_response(const Variant &p_id, int64_t p_token, const Ref<MCPTool> &p_tool, const String &p_checkpoint) override;
	virtual String get_project_path() const override;
	virtual String get_project_name() const override;
	virtual String get_editor_version() const override;

	// State directory shared with godot-ai-relay ($GODOT_AI_HOME, else ~/.godot-ai).
	static String get_state_dir();
	static String get_instances_dir();

	int get_http_port() const { return http_port; }
	String get_http_token() const { return http_token; }
};

#endif // MCP_SERVICE_H
