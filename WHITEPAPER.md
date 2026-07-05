# The Embedded Lua Aperture: MCP Access for Software That Already Embeds Lua

## Abstract

Many mature applications already embed Lua as their extension language. They
expose commands, callbacks, state, and domain objects to Lua so users can
automate host behavior without changing the host source tree. `liblua-mcp`
explores a direct question: what if an agent could enter that existing embedded
Lua world through the Model Context Protocol (MCP)?

The project promise is not a custom MCP server for Nmap, HAProxy, mpv, or any
other individual host. The promise is placement. If a host embeds this Lua
runtime, the MCP listener runs inside the Lua state the host already populated.
An MCP client can inspect bounded Lua-visible state, call Lua-visible functions
and methods under a control gate, and optionally use explicit wrappers when a
stable schema or narrower policy is useful.

That is the practical magic. `liblua-mcp` does not need built-in knowledge of
Nmap to be useful inside Nmap. Nmap's Lua environment already contains Nmap
meaning. The same is true for HAProxy's Lua `core` API, mpv's Lua API, and many
other Lua-embedded tools. `liblua-mcp` supplies the protocol aperture into that
world while the host keeps its normal input, output, lifecycle, and embedding
model.

Nmap is a useful case study because its Nmap Scripting Engine (NSE) is already
Lua-based and already used to automate network discovery and service analysis.
In a local proof, an MCP client reached a live Nmap process through
`liblua-mcp`, invoked a Lua-exposed proof tool, caused an evidence-producing
Nmap scan, and verified the scan result while the MCP call was in flight. The
proof is small and deliberately local, but it demonstrates the larger
architectural point: embedded Lua can become a local agent-facing control plane
for host capabilities that Lua can already reach, without designing a bespoke
MCP server for every host.

## 1. Problem

Modern agent systems need stable interfaces to software. The common pattern is
to write a new MCP server next to an existing application: a separate process
that shells out, calls an HTTP API, talks to a database, or scrapes some
reachable surface. That can work, but it often misses the strongest integration
point in applications that already embed a scripting language.

Lua is frequently embedded precisely because a host wants to be programmable.
The host decides what Lua can see: commands, events, configuration, network
state, media controls, scan context, proxy state, game APIs, editor commands,
or other domain-specific objects. That Lua boundary is not accidental. It is a
reviewed extension surface shaped by the host's existing architecture.

The missing piece is a standard way for agents to discover and invoke
Lua-visible operations. Without that, each host needs its own agent bridge,
each bridge has to rediscover host semantics from the outside, and each
implementation must decide how much of the host is safe to expose.
`liblua-mcp` treats the embedded Lua surface itself as the natural place to draw
the line: MCP can reach what the host already made visible to Lua, subject to
local mode gates and any additional wrapper policy the Lua side chooses to add.

## 2. Background

Lua is designed to be embedded. The official Lua documentation describes the
language, standard libraries, and C API; the C API allows host C code to
interact with Lua, register functions callable by Lua, and call Lua functions
from the host. In practice, hosts use that API to create a scripting surface
that is smaller and more domain-specific than the full host internals.

MCP provides a standard protocol for connecting clients and models to external
context and tools. MCP tools have names, descriptions, and input schemas, and
clients can list and invoke them. That makes MCP a good fit for explicit host
operations, as long as the exposed operations are bounded and auditable.

Nmap's NSE supplies the case-study host. The Nmap documentation describes NSE
as a Lua-based scripting engine for automating networking tasks, running scripts
in parallel with Nmap's scan engine. Nmap therefore already has the two
ingredients this experiment needs: a real application with useful behavior, and
an embedded Lua environment through which some of that behavior is scripted.

## 3. Architecture

`liblua-mcp` is a Lua fork that adds a built-in Lua module named `mcp`, loaded
with the normal Lua libraries. The library is disabled unless the host process
sets `LUA_MCP_ENABLE=1`. A Lua script can start a local listener with
`mcp.serve` or `mcp.listen`. A client can reach that listener through a local
Unix socket, or through the provided stdio bridge when the client expects stdio
MCP transport.

The runtime exposes three host-agnostic aperture tools:

- `lua_value_inspect`: resolve a dot-separated global path and return bounded
  type, shape, and value data.
- `lua_function_call`: in control mode, call a Lua-visible function by global
  path.
- `lua_method_call`: in control mode, call a Lua table method by receiver path
  and member name.

Lua code can still call `mcp.expose_tool(name, schema, fn, opts)` to register
explicit MCP tools. That remains valuable when a host operation needs a stable
name, schema, guardrail, or friendlier return structure. It is no longer the
only way the MCP side can learn from the host's Lua world.

The core architecture is intentionally local and layered:

```mermaid
flowchart LR
    A[Agent or MCP client] --> B[stdio bridge]
    B --> C[local liblua-mcp socket]
    C --> D[embedded Lua mcp library]
    D --> E[generic inspect/call tools]
    D --> G[optional explicit wrappers]
    E --> F[host APIs visible to Lua]
    G --> F
```

The host application does not need to speak MCP on stdout. Its normal output
remains parseable as normal host output. The MCP listener is a side channel
owned by the Lua runtime and located inside the same Lua environment the host
uses for scripting.

There are two integration paths:

- Replacement path: build or relink a host against `liblua-mcp`, then use the
  host's existing Lua loading mechanism to run an activator script.
- Host-cooperative path: a host includes `lmcp.h`, registers selected C-backed
  operations as MCP tools, and calls the liblua-mcp serve API directly.

The replacement path is useful because it can validate the model without asking
the host project to change its source code. The activator can rely on the
generic aperture tools, add explicit wrappers, or both. The host-cooperative
path is cleaner for a first-class mode such as a future `nmap --mcp`, because
the host can own lifecycle, initialization, and command scheduling directly
while still delegating MCP transport and tool plumbing to `liblua-mcp`.

## 4. Nmap Case Study

The Nmap proof used the replacement path. A custom Nmap build linked against a
Lua 5.4.8-compatible `liblua-mcp` branch. An NSE activator script started a
local liblua-mcp listener from inside Nmap's embedded Lua runtime. A Python
harness then used `tools/lua_mcp_stdio_bridge.py` to speak MCP over stdio while
the bridge forwarded requests to the Nmap-owned Unix socket.

The proof did not depend on an external Nmap MCP server. The agent reached a
live custom Nmap process through MCP, listed the Lua/MCP tools available in
that process, invoked an NSE-side proof operation, and received metadata about
the scan result. In current `liblua-mcp`, the generic aperture tools are the
substrate for this model: they let an agent inspect and call Lua-visible paths
without first writing a host-specific MCP protocol server.

The scan target was restricted to loopback proof services. One service returned
a distinctive TCP banner; the other served a small HTTP page with a distinctive
title. This kept the proof free of third-party network effects while still
exercising the Nmap paths that matter for agentic service analysis: service
version detection, NSE script execution, trace capture, and structured output.
The proof harness recorded MCP traffic separately from Nmap output so that the
control interaction could be evaluated independently from the scan result.

```mermaid
sequenceDiagram
    participant Agent as MCP client
    participant Bridge as lua_mcp_stdio_bridge.py
    participant Listener as liblua-mcp listener in Nmap
    participant NSE as NSE Lua proof tool
    participant Scan as child Nmap trace scan
    participant Analysis as post-scan analysis

    Agent->>Bridge: initialize, tools/list
    Bridge->>Listener: forward MCP requests
    Listener->>NSE: expose Lua runtime aperture
    Agent->>Bridge: inspect or call Lua-visible path
    Bridge->>Listener: tools/call
    Listener->>NSE: invoke Lua-visible operation
    NSE->>Scan: launch gated local trace scan
    Scan->>Analysis: produce normal Nmap outputs
    Analysis-->>Agent: summarize services and evidence
```

The local proof recorded a passing assertion set. The MCP transcript showed
client initialization, tool listing, and a proof-tool call. The scan produced
normal Nmap outputs and a separate evidence timeline. The timeline showed host
outputs appearing before the MCP scan call returned, which is important: the
agentic interaction and the host-generated evidence were concurrent parts of
the same operation.

The Nmap result identified both local proof services. One was a plain TCP
service with a distinctive banner, and the other was an HTTP service whose
title was extracted by NSE. That is enough to show agent-triggered service
analysis through the Lua-mediated path without publishing lab identifiers,
private network targets, or raw trace material in the public repository.

The key result is not merely that an agent can run a command named `nmap`.
Command execution alone would not prove the project thesis. The result is that
an MCP client can reach an Nmap-owned embedded Lua runtime, discover what that
runtime exposes, invoke Lua-mediated host work through liblua-mcp, and then
analyze host-generated artifacts. That is the control-plane pattern.

The evidence separates three layers that are often conflated in agent
integrations. The MCP transcript proves client-to-tool interaction. The exposed
Lua tool proves that the host's embedded Lua runtime mediated the request. The
Nmap artifacts prove that host work happened and produced normal Nmap outputs.
Keeping those layers distinct makes the experiment auditable and helps avoid
overstating what has been proven.

## 5. Security Boundary

The project boundary is deliberately narrower than unrestricted process
control.

First, `liblua-mcp` is disabled by default. The host process must opt in with
`LUA_MCP_ENABLE=1`. Control tools require `LUA_MCP_CONTROL=1`. Hazardous tools,
such as eval-style Lua execution, require a separate loud acknowledgement gate.

Second, observe mode and control mode are different. Observe mode is for
bounded runtime discovery. Control mode can invoke Lua-visible functions and
methods, so it must be treated as trusted local control. If a host's Lua state
contains powerful functions, control mode can trigger powerful host behavior.
Explicit `mcp.expose_tool` wrappers remain the preferred way to narrow complex
or risky operations into reviewed tool contracts.

Third, hazard mode is separate. Eval-style execution and direct global mutation
remain behind a louder acknowledgement gate. Control mode is still powerful,
but it does not by itself grant dynamic code evaluation through the hazard
tools.

Fourth, process-level launchers are a distinct risk class. The current Nmap
adapter includes gated local-lab tools that can spawn a local Nmap subprocess
for agent-requested scans or NSE scripts. Those tools are useful for proof and
experimentation, but they are not the same as embedded state inspection. They
must remain separately gated, explicit, and auditable.

Fifth, transport is local. The initial transport is a Unix socket with
restrictive permissions, with stdio available through a bridge process. This is
appropriate for trusted local lab work and architectural review. It is not a
production security claim.

## 6. Implications for Lua Embedders

For Lua embedders, the most important implication is that MCP integration does
not have to begin as a new host-native protocol implementation. If a host
already embeds Lua and already exposes useful operations to Lua, `liblua-mcp`
can provide a generic agent path into that Lua-visible surface.

That produces a useful adoption ladder:

1. Prove the concept with the replacement path and a Lua activator.
2. Use generic inspection to understand the host's Lua-visible shape.
3. Call existing Lua-visible functions or methods in trusted control mode.
4. Move recurring or risky operations into reviewed wrapper tools with schemas.
5. If the integration is valuable, consider a host-cooperative mode that calls
   `lmcp.h` directly.

The host-cooperative mode is the cleaner long-term form. For Nmap, that could
mean a mode that initializes NSE Lua, registers scan and script operations,
starts a local MCP listener, and waits for agent input. In that design, Nmap
would own its lifecycle and execution policy, while `liblua-mcp` would provide
the MCP transport, request parsing, and tool dispatch.

The same pattern can apply outside network scanning. HAProxy can expose
selected proxy and server state through its Lua `core` API. mpv can expose
player state and controls through its Lua API. Game engines, editors, test
harnesses, observability agents, and automation-heavy tools can use the same
shape when Lua is already their extension layer.

## 7. Future Work

The current alpha should be treated as trusted local research infrastructure.
The next work is to make the aperture more useful and the boundary easier to
review:

- improve generic path inspection for tables, metatables, modules, callable
  values, and host-shaped objects;
- support structured JSON arguments as Lua tables for generic function and
  method calls;
- stabilize tool schemas, structured returns, and error reporting;
- improve local socket discovery without weakening locality;
- keep host-specific behavior in adapters unless it belongs in the generic
  Lua aperture;
- document replacement-path and host-cooperative patterns separately;
- expand proof targets only when they show a new host/Lua relationship;
- develop stronger safety guidance for process-level launchers.

The Nmap case study is enough to justify further work because it proves the
mechanism under a real Lua-embedded host. It is not enough to claim production
readiness. The value of the experiment is architectural: embedded Lua can be
more than a plugin language. With explicit gates, generic inspection, generic
calls, and optional reviewed wrappers, it can be a local MCP control layer for
software that already chose Lua as its programmable surface.

## References

- Model Context Protocol specification, version 2025-06-18:
  <https://modelcontextprotocol.io/specification/2025-06-18>
- Model Context Protocol tools documentation:
  <https://modelcontextprotocol.io/specification/2025-06-18/server/tools>
- Lua documentation: <https://www.lua.org/docs.html>
- Lua C API overview: <https://www.lua.org/pil/24.html>
- Nmap Scripting Engine manual: <https://nmap.org/book/man-nse.html>
- Nmap NSE language documentation: <https://nmap.org/book/nse-language.html>
