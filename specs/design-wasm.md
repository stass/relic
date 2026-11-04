# WebAssembly interface

The WebAssembly interface runs the ordinary relic library in a browser worker
and returns owned JavaScript values. It is a reusable interface beside the C
library and command-line tool; the website playground is one consumer. The
adapter follows the public C API and adds bounded copying, an asynchronous
operation, and cancellation without changing Datalog evaluation.

Related: [library interface](design-api.md), [website](design-website.md),
[memory](design-memory.md), [architecture contracts](architecture.md#contracts).

## Model

The public ES module exports `evaluate(program, options)`, `RelicError` and a
frozen `limits` object describing the shipped ceilings. The program is a
JavaScript string containing facts, rules and queries ending in `?`. A call
returns a promise for an object containing the engine version, an ordered
array of query answers, and diagnostics. Each query answer contains `rows`,
an array of arrays of typed values. Queries appear in source order. A program
with no queries is loaded and checked and returns an empty query array; it
does not materialize unused relations.

Each call owns a fresh worker, Wasm instance and C context. The worker loads
and checks the complete program, obtains its queries through
`relic_query_count()` and `relic_query_loaded()`, and runs them in order through
`relic_query_run()`. It destroys each query before continuing and destroys the
context before publishing success. The public module terminates the worker
when the call settles, whether successfully or unsuccessfully.

There is one operation and no public context, query handle, borrowed result,
explicit disposal, queue or reusable worker pool. Repeated calls have no shared
engine state. Applications control how many calls they start concurrently.
The playground permits one active call. This model trades worker startup and
program reload cost for a single lifetime that also handles cancellation,
failure and the engine's context-lifetime interned values.

The distributable directory contains `relic.mjs`, `relic.d.ts`,
`relic-worker.mjs`, `relic-engine.mjs`, `relic-engine.wasm`, a README and license
notices. Relative module URLs locate the companion
assets, so an embedder serves the directory and imports the public module
without a bundler or package installation. The README documents the API,
limits, required browser features, asset serving and a complete embedding
example. Worker messages and C bridge entry points are private interfaces.

### Public JavaScript contract

The declarations implement these shapes. All failures reject the promise with
`RelicError`; validation does not throw synchronously from `evaluate()`.
Omitted options use the profile defaults. Unknown option keys are rejected,
so a misspelled limit cannot silently select a larger default.

```typescript
export type Value =
    | { type: "integer"; value: bigint }
    | { type: "symbol"; value: string }
    | { type: "string"; value: Uint8Array };

export interface Diagnostic {
    level: "note" | "warning" | "error";
    message: string;
    location: { file: string | null; line: number; column: number } | null;
}

export interface Evaluation {
    version: string;
    queries: { rows: Value[][] }[];
    diagnostics: Diagnostic[];
}

export interface Options {
    signal?: AbortSignal;
    timeoutMs?: number;
    maxInputBytes?: number;
    maxMemoryBytes?: number;
    maxTuples?: number;
    maxMagic?: number;
    maxRows?: number;
    maxOutputBytes?: number;
    maxDiagnosticBytes?: number;
}

export type ErrorKind = "argument" | "rejected" | "evaluation" | "limit"
    | "memory" | "internal" | "cancelled" | "timeout" | "worker";

export class RelicError extends Error {
    readonly kind: ErrorKind;
    readonly engineClass: number | null;
    readonly diagnostics: Diagnostic[];
}

export const limits: Readonly<Required<Omit<Options, "signal">>>;
export function evaluate(program: string, options?: Options): Promise<Evaluation>;
```

The C classes `REJECTED`, `EVALUATION`, `LIMIT` and `MEMORY` map to the
corresponding lowercase kind. `INTERRUPTED` maps to `cancelled`. `ARGUMENT`,
`IO` and `INTERNAL` map to `internal`: validated browser input cannot cause a
malformed C call or a filesystem operation, so these indicate an adapter or
engine defect. `NONE` on a failed C operation, and an unknown C class, also
map to `internal`. `engineClass` holds the numeric C enumerator on a C failure
and is otherwise null. JavaScript input validation uses `argument`; bridge
budget exhaustion uses `limit`; abort, elapsed deadline and worker/runtime
failures use `cancelled`, `timeout` and `worker`, respectively. No caller needs
to match diagnostic text to classify failure.

## Invariants

* The C adapter includes `relic.h` and uses only its public types and calls.
  It includes no engine-private headers and exports no engine data layout to
  JavaScript. C reads C structures and calls public value accessors.
* All engine work runs in a dedicated worker. The UI thread only validates,
  manages the worker lifetime and consumes the copied reply.
* A successful reply contains every row of every query. An engine error,
  bridge error or exceeded output limit rejects the entire call; no prefix is
  represented as a successful answer.
* No pointer, interned identifier or view into Wasm memory crosses the public
  boundary. Returned values survive worker destruction and subsequent calls.
* Every operation has finite source, engine, copied-output and time limits.
  A browser embedding cannot disable them through a zero or infinite value.
* Engine semantics and ordinary native builds do not depend on the website,
  JavaScript, Emscripten or browser hosting.

## Values and diagnostics

Each cell is a tagged value: an integer contains a JavaScript `bigint`, a
symbol contains a JavaScript string, and a relic string contains an owned
`Uint8Array`. The declarations define the discriminated union and its field
names. Integer conversion preserves the full signed 64-bit range without an
intermediate JavaScript `number`. The module enables Emscripten's BigInt
integration or uses an equivalently exact conversion at the C boundary.

Symbols follow relic's ASCII identifier grammar. Relic strings are arbitrary
bytes, including NUL and invalid UTF-8, so decoding them to JavaScript strings
would change their meaning. The row callback obtains bytes with
`relic_string()` and its returned length and copies them before returning.
It refreshes any private Wasm memory view after a call that can grow memory.
Transferable buffers may move copied data between worker and caller; they
never refer to the engine's linear memory.

A query that has no answers has `rows: []`. A true query with no output
variables has `rows: [[]]`; false has `rows: []`. Column names and the column
count of an empty answer are not part of this interface. The adapter needs only
the cells supplied by the public row callback, avoiding a second whole-answer
copy and access to internal query structures.

Diagnostics contain severity, message and an optional source location. The
adapter copies the borrowed message and location during the diagnostic
callback, including the location's source name. Locations retain the C API's
one-based line and column convention. Diagnostic messages are display text;
decoding malformed UTF-8 uses replacement characters. This does not affect
the byte-preserving representation of string values. The source name is a
fixed label for the submitted program rather than an arbitrary filesystem
path.

The public error object carries the bounded diagnostics available when an ordinary
operation fails and the C error class when applicable. The bridge reads that
class immediately after a failing C call; a previous diagnostic is not evidence
that a later call failed. A worker terminated during evaluation cannot provide
diagnostics it has not sent, so cancellation and timeout do not promise them.

## Build and distribution

Emscripten is an optional development toolchain. The Wasm build configures a
separate build directory with `--host=wasm32-unknown-emscripten`,
`--disable-shared`, `--disable-threads` and `--with-libedit=no`. It uses the
ordinary autotools library target and its source selection, then links a thin
C adapter into a modular ES module and `.wasm` artifact. It does not maintain
a second list of engine sources or compile the command-line executable as the
JavaScript API. Target-specific adapter code is selected by this build, without
platform branches in the engine.

The link enables memory growth with a finite maximum and a fixed stack budget;
allocation failure remains reportable rather than deliberately aborting the
runtime. The engine uses one thread. There are no pthreads, shared Wasm memory,
`SharedArrayBuffer`, Asyncify transformations or cross-origin-isolation header
requirements. The generated support code and toolchain libc accompany the
Wasm artifact; they introduce no dependency into the native library or tool.
The public module uses browser-standard APIs and no npm runtime package.

The deployment serves modules and Wasm with appropriate content types from
the same origin. Source text and results stay in the browser. The adapter
exposes no filesystem import, native path, network request or arbitrary C
entry point. The trusted module fetches its companion assets; user program
text selects none of those URLs. A Wasm instance is memory isolation for the
engine, not a security boundary against a compromised script on the host page.

## Failure paths and limits

The public module validates options and input before creating a worker or
posting source text. Source length is measured in UTF-8 bytes, not UTF-16 code
units; a preliminary code-unit bound prevents an oversized encoding
allocation. Literal NUL is rejected because `relic_load_string()` takes a
NUL-terminated buffer. Escaped NUL inside a relic string literal remains valid.
Unpaired UTF-16 surrogates are rejected so encoding cannot silently replace
the caller's source. The encoded source includes every embedded query, so
there is no separate query-text path that bypasses the source limit.

The shipped profile defines these finite defaults and ceilings. Byte units
are binary. Module constants own the numeric profile; the README publishes
the same values and units.

| Option | Default and ceiling | Unit |
| --- | --- | --- |
| `maxInputBytes` | 1,048,576 | UTF-8 bytes |
| `maxMemoryBytes` | 67,108,864 | Engine-accounted bytes |
| `maxTuples` | 100,000 | Engine tuples |
| `maxMagic` | 100,000 | Rewrite work units |
| `maxRows` | 10,000 | Returned rows across all queries |
| `maxOutputBytes` | 8,388,608 | Payload bytes plus structural charges |
| `maxDiagnosticBytes` | 65,536 | Diagnostic bytes plus structural charges |
| `timeoutMs` | 5,000 | Elapsed milliseconds |

The Wasm memory maximum is 256 MiB and its reserved stack is 1 MiB; these are
build constants rather than options. A caller
can select a smaller positive safe integer for a configurable limit. A value
above the shipped ceiling, zero, a fraction, NaN or infinity is rejected.

| Resource | Enforcement owner | What it bounds |
| --- | --- | --- |
| Source bytes | Public module and C configuration | Encoded program, including queries; `max_input` provides the engine check |
| Wasm memory and stack | Link configuration | Linear memory maximum and reserved C stack |
| Accounted engine memory | `relic_config_t.max_memory` | Engine-accounted storage and working state |
| Engine tuples | `relic_config_t.max_tuples` | Facts, derived relations and gathered answers |
| Rewrite work | `relic_config_t.max_magic` | Magic rewrite effort, with ordinary fallback semantics |
| Returned rows | Bridge | Aggregate rows across all queries |
| Copied output bytes | Bridge | Payload and structural charges across all query answers |
| Diagnostic bytes | Bridge | Copied messages, source labels and structural charges |
| Elapsed time | Public module | Worker creation, module loading and evaluation through reply |

`max_memory` is not a bound on all engine allocations or peak memory. The Wasm
maximum covers linear memory, including allocations the engine account omits;
it does not cover the browser's JavaScript heap, compiled code or runtime
overhead. Copy budgets bound the data the bridge retains, rather than claiming
an exact process-memory ceiling. They charge a fixed positive amount for each
query, row, cell and diagnostic as well as its payload. Thus empty answers,
zero-column rows and empty byte strings cannot create unbounded object counts
under a zero payload-byte charge. Checks precede allocation and copying and use
overflow-safe arithmetic. The diagnostics budget is independent of the result
budget, so errors remain reportable after output fills.

On a row-copy failure the callback records a sticky bridge failure and asks
`relic_query_run()` to stop. A callback stop is success in the C API, so the
adapter checks its own failure before publishing a query or starting the next
one. The failure discards all copied query answers. A diagnostic-copy failure
also sets that failure and retains no further diagnostics. Loading and checking
may run to their normal return because diagnostic callbacks cannot stop every
operation; subsequent stages observe the failure and do not start. Cleanup
releases every query and context on ordinary failure. A trap, exhausted stack,
worker startup failure or failed asset load becomes a module/worker failure,
and the owning module terminates that worker.

`options.signal` accepts an `AbortSignal`; `options.timeoutMs` specifies a
finite deadline within the profile. An already-aborted signal rejects before
worker creation. Abort and timeout terminate the worker and reject the promise.
The worker cannot service a message while synchronous C code occupies its
event loop, so cancellation does not post an interrupt command or rely on
`relic_interrupt()`. Termination also covers parsing, checking and module
startup. A fresh call creates a fresh worker after any failure. Success,
worker failure, abort and deadline race through one settle operation that
clears the timer, removes listeners and prevents a late reply from changing
the outcome. Browser scheduling can delay timers; the deadline is a best-effort
elapsed-time limit, not a real-time guarantee.

## Determinism

Successful answers preserve the native library's value types, query order and
row order for the same engine version, input and single-thread configuration.
The interface does not sort answers or promise a particular sort order.
Tests compare typed values,
not JSON numbers or a lossy UTF-8 rendering of strings. Worker startup time,
wall-clock cancellation and resource exhaustion are environmental and do not
promise identical failure timing across browsers. A successful bounded result
contains the same answers as a native run with sufficient resources.

## Validation

CI pins the Emscripten SDK and builds the artifact from the checkout whenever
engine, compatibility, public API, build, bridge or playground inputs change.
The website deployment uses that build's complete artifact directory; it does
not reuse an unchecked binary from another revision. Native builds and tests
remain independent of the optional toolchain.

Relevant C API suites execute as Wasm, and browser tests exercise the public
module and the playground. Agreement fixtures run natively and through the
browser API: recursion, negation, aggregates, arithmetic, regular expressions,
multiple queries, no queries, empty answers, zero-column answers, signed
64-bit extremes and strings containing NUL and non-UTF-8 bytes. Bridge tests
cover rejected source and options, UTF-8 byte limits, literal NUL, unpaired
surrogates, engine limits, aggregate row and byte limits, diagnostic exhaustion,
missing assets, abort before startup, abort during work, timeout and successful
calls after each failure. Output objects remain valid after worker termination
and after a later call. Hostile source and diagnostics render as text in the
playground. A long-running fixture verifies that the page remains responsive
and Stop settles the call without waiting for the C operation to return.

## Alternatives not taken

A persistent context API requires users and workers to coordinate handles,
query/result lifetimes, accumulated interned values, cancellation recovery and
disposal. A single operation makes those states private to one lifetime.

A main-thread API blocks rendering and cancellation. A message-based interrupt
cannot run while the worker is executing C. Shared memory or resumable Wasm
would add synchronization or transformed execution for a capability supplied
by terminating the operation's worker.

CLI-output parsing loses value types and makes diagnostics part of a text
protocol. Returning JSON numbers loses exact integers; decoding every string
as UTF-8 loses bytes. The typed, copied interface avoids all three conversions.

A JavaScript Datalog engine would duplicate semantics and validation. The
adapter and playground use the same C library as native clients.
