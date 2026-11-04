# WebAssembly feasibility

This experiment checks whether the existing C library can execute as Wasm
before implementing the [WebAssembly interface](../specs/design-wasm.md).
The experiment uses Emscripten 6.0.13 on Apple silicon macOS, Node.js and a
headless Brave browser served from localhost. It changes no engine source.
Disposable sources and artifacts live under `disposable/wasm-probe/` and are
not release artifacts.

## Results

| Check | Result |
| --- | --- |
| Native `make -j8 check` | 332 passes, no skips |
| Static Wasm library build, threads and libedit disabled | Pass |
| Explicit `wasm32-unknown-emscripten` host configuration and library build | Pass with a generated Libtool probe diagnostic |
| C API unit suite as Wasm | Pass in Node and browser |
| Arithmetic unit suite as Wasm | Pass in Node and browser |
| Value unit suite as Wasm | Pass in Node and browser |
| Query unit suite as Wasm | Pass in Node and browser |
| Regular-expression unit suite as Wasm | Pass in Node and browser |
| `examples/query.c` as Wasm in browser | Prints `b` and `c` |

The native run initially encountered sandbox restrictions in `tests/cli.sh`
and `tests/terminal.sh`. Both pass with access to `/dev/stdout` and
`/dev/tty`; the completed suite summary reports all 332 tests passing.

The Wasm configuration detects a declared, linkable `getentropy()` and does
not select the `/dev/urandom` compatibility implementation. Successful
context construction in both runtimes exercises the library's secure hash
seeding requirement. `RELIC_THREADS` is undefined. The interrupt flag's
remaining C11 atomic operations compile and execute in this configuration.

The existing suites exercise C API diagnostics, ownership and refusal paths,
recursive queries, resource limits, value bytes and 64-bit arithmetic on the
wasm32 target. They establish portability of these tested engine paths, not
correctness of a JavaScript binding that has yet to be implemented.

The browser query smoke test produces a 237,883-byte Wasm file and 9,103 bytes
of generated JavaScript before compression. These sizes describe that small
C client linked at `-O2`; they are not a size estimate for the public module
or the playground. Browser performance has no benchmark result from this
experiment.

## Reproducing the C tests

Use a disposable copy of the sources containing the generated `configure`,
`Makefile.in`, `config.h.in` and supporting Autotools files, or unpack a
release source tarball. The commands below run inside that copy. Emscripten
is a development tool; on macOS it can be installed with
`brew install emscripten`.

Set `EM_CACHE` to a canonical writable directory when the installed SDK's
default cache is outside the writable build area. The experiment uses
`disposable/em-cache/` under the repository's absolute path.

```sh
emconfigure ./configure --host=wasm32-unknown-emscripten \
    --disable-shared --disable-threads \
    --with-libedit=no CFLAGS=-O2
emmake make -j8 librelic.la

for suite in api arith value query regex; do
    emcc -O2 -I. -Iinclude -Ilib -Icompat -Itests/unit \
        "tests/unit/test_$suite.c" tests/unit/harness.c \
        .libs/librelic.a -sALLOW_MEMORY_GROWTH=1 \
        -sMAXIMUM_MEMORY=268435456 -sSTACK_SIZE=1048576 \
        -o "test-$suite.cjs" || exit 1
    node "test-$suite.cjs" || exit 1
done
```

Both the invocation above and an invocation without `--host` build the
library. The five Node suites also pass after explicitly selecting the host.
The explicit-host configure run emits a nonfatal `=-fPIC: command not found`
diagnostic from the generated Libtool C++ probe. The generated Emscripten case
contains a bare `='-fPIC'` assignment expanded from `_LT_COMPILER_PIC($1)` in
the tool's `libtool.m4`. This build-tool integration issue is recorded with
the Wasm implementation work in [TODO.md](../TODO.md); the C library build
and test execution succeed despite it.

For browser execution, compile the same suites with `-sEXIT_RUNTIME=1` and
output names such as `browser-api.js`. Serve the generated files over local
HTTP. Load each generated script after this HTML harness, changing its `src`
to name the suite:

```html
<!doctype html>
<meta charset="utf-8">
<title>Wasm unit suite</title>
<pre id="output"></pre>
<script>
var Module = {
    print: (line) => document.getElementById('output').textContent += line + '\n',
    printErr: (line) => document.getElementById('output').textContent += line + '\n',
    onExit: (status) => document.body.dataset.status = status,
    onAbort: (reason) => document.body.dataset.status = 'abort: ' + reason
};
</script>
<script src="browser-api.js"></script>
```

The experiment drives an isolated headless Brave profile through the local
Chrome DevTools Protocol. Each of the five pages reports
`document.body.dataset.status === "0"` with empty output. This runs the
compiled test suites inside the browser rather than simulating browser APIs
in Node.

## Implementation validation

The [design's validation requirements](../specs/design-wasm.md#validation)
cover the public adapter, copied JavaScript values, browser worker lifecycle,
output budgets, cancellation and playground rendering. Those mechanisms are
outside this experiment. Their tests must exercise the actual public module
and website, including failure and recovery, before the browser feature ships.
