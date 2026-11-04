# relic for JavaScript

Run Datalog locally in a browser or Node.js. The module uses the same C
engine as the native tool, with one thread. It has no npm runtime dependency.

## Build

Install Emscripten, Python 3 and the native build tools. On macOS, use
`brew install emscripten autoconf automake libtool`. From a source checkout:

```sh
./autogen.sh
./configure
make wasm-build
```

The distribution is written to `disposable/wasm-dist/`. Choose another
destination with `make wasm-build WASM_OUTPUT="$PWD/website/assets/wasm"`.
Generated modules and binaries are not source files. Copy the complete output
directory, including its licenses, when embedding or redistributing relic.

`make wasm-check` also runs five C unit suites as Wasm and the public
JavaScript API tests against native agreement fixtures. For a real-browser
run, build into `website/assets/wasm/` and run
`node tests/wasm/browser-tests.mjs /path/to/chrome` from the repository root.

## Use

Serve the directory over HTTP(S), with `.mjs` as JavaScript and `.wasm` as
`application/wasm`. Browser execution requires ES modules, WebAssembly,
dedicated workers, `BigInt`, `AbortController`, and standard text encoders.
Cross-origin-isolation headers are not required. Assets are resolved relative
to the public module. Import it directly, without a bundler:

```javascript
import { evaluate, RelicError } from "./relic.mjs";

const controller = new AbortController();
try {
    const result = await evaluate(`
        edge(a, b). edge(b, c).
        path(X, Y) :- edge(X, Y).
        path(X, Z) :- edge(X, Y), path(Y, Z).
        path(a, X)?
    `, { signal: controller.signal });
    console.log(result.version, result.queries[0].rows);
} catch (error) {
    if (error instanceof RelicError)
        console.error(error.kind, error.message, error.diagnostics);
}
```

Node.js uses its built-in workers and loads the same artifacts from disk.
Use Node.js 22 or newer with the browser-compatible APIs listed above.
In a page, render results and diagnostics with text nodes or `textContent`.

Every call owns a fresh worker and context. Abort the signal to stop it; the
worker is terminated on every outcome. Results own their bytes and remain valid
after completion. Repeated calls share no program state. There is no disposal
operation. Applications choose how many evaluations they start concurrently.

## Values and failures

Each query in source order returns `{ rows: Value[][] }`. Integer cells are
`{ type: "integer", value: bigint }`; symbols have `type: "symbol"` and a string
value; relic strings have `type: "string"` and an owned `Uint8Array` value.
Strings preserve arbitrary bytes, including NUL and invalid UTF-8. Answers are
not sorted. No answers is `[]`; a true query without output variables is `[[]]`.
Programs without queries are loaded and checked but do not materialize rules.

Diagnostics have `level`, `message`, and a nullable `location` with `file`,
one-based `line` and `column`. All errors reject the promise with `RelicError`;
`kind` is `argument`, `rejected`, `evaluation`, `limit`, `memory`, `internal`,
`cancelled`, `timeout` or `worker`. `engineClass` is the numeric C error class
when available, otherwise null. The error contains bounded diagnostics; abort
and timeout cannot retrieve diagnostics from a busy worker. Failure discards
every query answer rather than returning partial success. A later call works
independently of any earlier failure.

## Limits

The frozen exported `limits` object contains these defaults and ceilings.
Options can lower each limit to a positive safe integer. Unknown options,
fractions, zero, infinity and values above the ceiling are rejected.

| Option | Default and ceiling |
| --- | ---: |
| `timeoutMs` | 5,000 milliseconds |
| `maxInputBytes` | 1,048,576 UTF-8 bytes |
| `maxMemoryBytes` | 67,108,864 engine-accounted bytes |
| `maxTuples` | 100,000 tuples |
| `maxMagic` | 100,000 rewrite work units |
| `maxRows` | 10,000 returned rows across queries |
| `maxOutputBytes` | 8,388,608 charged output bytes |
| `maxDiagnosticBytes` | 65,536 charged diagnostic bytes |

Output charges 32 bytes per query, row and cell, plus symbol/string payload
bytes. Diagnostics charge 64 bytes per diagnostic plus message and source-name
bytes. These budgets bound retained data, not exact JavaScript heap usage.
The engine memory account does not cover every allocation. The Wasm runtime
has a separate 256 MiB linear-memory ceiling and a reserved 1 MiB stack;
JavaScript, compiled code and browser overhead are outside that ceiling.
Exceeding rewrite work invokes the engine's normal fallback and can still
succeed. The elapsed deadline includes worker startup and asset loading;
browser scheduling can delay timers.

Source includes all embedded queries. Literal NUL and unpaired UTF-16 surrogates
are rejected; write a relic escape to express NUL inside a string literal.
No filesystem, import path or network operation is exposed to Datalog. The
runtime loads companion assets, but programs and answers are not uploaded.
