---
inclusion: manual
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
# Debug build with symbols
cmake -B out -DCMAKE_BUILD_TYPE=Debug

# With ASAN for memory error detection
cmake -B out -DXYLEM_ENABLE_ASAN=ON
```

## MCP Remote Debugging (AI-Assisted)

> **Note for AI agents:** When the user asks about debugging, check if the relevant MCP debugger server is enabled in the project's MCP configuration. If it is disabled, ask the user if they want to enable it and ensure dependencies are installed before proceeding.

Start a debug server, then tell the AI agent to connect. The agent handles breakpoints, stepping, variable inspection, etc. via natural language.

### Windows (CDB)

```bash
# Start CDB remote debug server (pick any free port)
cdb -server tcp:port=5005 -o out\Debug\<program>.exe
```

Then tell the agent to connect with: `tcp:Port=5005,Server=localhost`

## Cross-Platform Pitfalls

- `long` is 4 bytes on Windows x64, 8 bytes on Linux/macOS x64 — watch for truncation
- Stack size defaults differ: Windows 1MB, Linux 8MB
- Uninitialized memory may be zeroed in debug builds but not release — use ASAN
- Signal handling differs: Windows uses SEH, Unix uses POSIX signals
- File path separators: `\` on Windows, `/` on Unix

## Tips

- Always reproduce with a minimal test case first
- Use sanitizers (ASAN/TSAN/UBSAN) before reaching for a debugger
- Check compiler warnings — many bugs are caught at compile time
- On Windows, use Debug configuration (`-C Debug`) for full symbol info
- Core dumps: `ulimit -c unlimited` on Linux, then `gdb ./program core`
