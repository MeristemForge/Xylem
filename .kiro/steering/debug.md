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

## Debug Build Configuration

```bash
cmake -B out -DCMAKE_BUILD_TYPE=Debug
cmake -B out -DXYLEM_ENABLE_ASAN=ON
```

## MCP Remote Debugging (AI-Assisted)

Start a debug server, then tell the AI agent to connect.

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

## Cross-Platform Pitfalls

- `long`: 4 bytes on Windows x64, 8 bytes on Linux/macOS x64
- Stack size: Windows 1MB, Linux 8MB
- Uninitialized memory may be zeroed in debug but not release — use ASAN
- Signals: Windows uses SEH, Unix uses POSIX signals
