def can_build(env, platform):
    # MCP server is editor-only. Dependencies:
    #   jsonrpc  - MCPProtocol inherits from JSONRPC.
    #   gdscript - GDScript validation tools use GDScriptLanguage::validate().
    env.module_add_dependencies("mcp_server", ["jsonrpc", "gdscript"])
    return env.editor_build


def configure(env):
    pass
