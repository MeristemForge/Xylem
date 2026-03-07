---
inclusion: manual
description: "Debugging guide with platform debuggers and MCP servers"
---

# Debugging Guide

## Platform Debuggers

| Platform | Debugger | MCP Server |
|----------|----------|------------|
| Windows | CDB/WinDbg | mcp-windbg (`pip install mcp-windbg`, requires Python >= 3.10) |
| Linux | GDB | mcp-gdb (`uvx mcp-gdb`) |
| macOS | LLDB | lldb-mcp (`git clone https://github.com/stass/lldb-mcp`) |

## MCP Remote Debugging (AI-Assisted)

> **Note for AI agents:** When the user asks about debugging, check if the relevant MCP debugger server is enabled in the project's MCP configuration. If it is disabled, ask the user if they want to enable it and ensure dependencies are installed before proceeding.

Start a debug server, then tell the AI agent to connect. The agent handles breakpoints, stepping, variable inspection, etc. via natural language.

### Windows (CDB)

```bash
cdb -server tcp:port=5005 -o out\Debug\<program>.exe
```

Then tell the agent to connect with: `tcp:Port=5005,Server=localhost`

### Linux (GDB)

```bash
gdb -ex "target remote :5005" ./out/<program>
```

### macOS (LLDB)

```bash
lldb ./out/<program>
```

## Tips

- Reproduce with minimal test case first
- Use sanitizers before reaching for a debugger
- On Windows, use `-C Debug` for full symbol info
- Core dumps on Linux: `ulimit -c unlimited`, then `gdb ./program core`
