# relic interrupt design

**Status.** Design document for stopping an evaluation part way: the entry
point a caller interrupts a context through, what an interrupted operation
returns and leaves behind, where the evaluator notices, and what the
interactive session of the tool does with Ctrl-C. Forward-looking, like every
document under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/design-api.md` sections 2, 6 and 11 for the interface
convention, re-entry and what is absent; `specs/design-eval.md` sections 7
and 8 for answering a query and how an evaluation fails;
`specs/design-parallel.md` section 4 for the pool and an abandoned job;
`specs/design-cli.md` section 10 for the interactive session;
`specs/decisions.md` D-14, D-25 and D-29.

---

## 1. What this document settles

* the entry point, `relic_interrupt()`, and from where it may be called;
* which interrupt an operation heeds, and which it forgets;
* what an interrupted operation returns, reports and leaves in the context;
* the rows a query may have delivered before it was interrupted;
* where the evaluator looks for an interrupt, and the bound on the work it
  does before it looks;
* what the threads of a shared round do when the context is interrupted;
* what the interactive session does with Ctrl-C.

It adds no configuration field and no limit. `relic_config_t` is unchanged.

---

## 2. The entry point

```c
void relic_interrupt(relic_t *ctx);
```

`relic_interrupt()` asks the operation running on `ctx` to stop. It raises a
flag the context holds and does nothing else: it does not enter the context,
allocates nothing, reports nothing and cannot fail. So it may be called

* from a signal handler, which is what the tool does;
* from a thread other than the one running the operation, which is how an
  embedder gives an evaluation a deadline;
* from caller code the operation calls -- a row, diagnostic or explain
  callback -- which is how the tests interrupt at a chosen point.

The flag is a lock-free atomic where the compiler has C11 atomics, and a
`volatile sig_atomic_t` where it does not; either is a store a signal handler
may make. Only the atomic may be stored to from another thread: without C11
atomics a store from a second thread races with the evaluation's loads, so
such a build supports an interrupt from a signal handler or a callback and
not from a thread. Every compiler of a system relic supports has them. The
caller keeps `ctx` alive across the call: a context destroyed under a
concurrent `relic_interrupt()` is a use after free, as any other call on it
would be. `relic_interrupt(NULL)` does nothing, so a handler installed
before its context exists is harmless.

**An interrupt reaches the operation running when it is made.** Entering an
operation lowers the flag, after the busy check, so a refused call lowers
nothing. An interrupt made while no operation runs -- at the tool's prompt,
between two calls of an embedder -- is therefore forgotten when the next
operation begins, rather than failing an operation that started after it. The
price is the window between two operations: an interrupt made after one
returns and before the next enters is lost. A caller that must not lose one
keeps its own record of the signal and checks it between calls, as the tool
does, section 5.

**Evaluation heeds an interrupt; nothing else does.** `relic_materialize()`,
`relic_query_run()` and `relic_query_collect()` look for it while they
evaluate and while they deliver rows, section 4. Loading, importing, adding a
fact, checking and preparing take time bounded by their input and the
program's size, and run to their end; an interrupt made during one is
forgotten when the next operation begins. An evaluation's time is bounded by
nothing the caller sets (`specs/design-api.md` section 11), which is why it
is the one that needs stopping.

---

## 3. What an interrupted operation leaves

An interrupted evaluation fails as an exceeded limit fails it
(`specs/design-eval.md` section 8): it returns -1, or NULL from
`relic_query_collect()`, with one RELIC_ERROR diagnostic naming what it was
doing,

```text
relic: interrupted while deriving path/2
relic: interrupted while deriving the query
relic: interrupted while delivering the query
```

and the context as any failed evaluation leaves it. The relation that was
being derived into holds every tuple inserted before the interrupt and
nothing half-inserted; every other relation holds tuples that are all
justified; the evaluation arena and the round's buffers are released; no
materialized program is recorded, so the next evaluation discards what this
one derived and starts again. Nothing needs unwinding, because the interrupt
is looked for only where the evaluator could already fail.

**Rows delivered before an interrupt stay delivered.** A query promises that
no row is delivered until everything that can fail has succeeded
(`specs/design-eval.md` section 7). An interrupt is the one failure that can
come after the first row: a query that streams is interrupted while it
derives, and a gathered answer while it is delivered. The run still returns
-1, so the caller learns that the rows it has are a prefix of the answer, and
the caller asked for that. Refusing the interrupt until the answer is
delivered would leave a query that streams a large answer, or one whose row
callback writes to a slow terminal, impossible to stop; gathering every
answer before delivering it would give up streaming for every context, since
any context can be interrupted.

A row callback that returns nonzero still stops the rows with success: the
caller asked for no more rows, and nothing failed. An interrupt is a failure
because the answer is incomplete and the caller did not choose where it
ended.

---

## 4. Where an evaluation notices

The flag is read where the evaluator already takes the executor's results,
so an interrupt joins the failures that can happen there rather than adding
a place that can fail.

**The executor returns at least every quantum of work.**
`relic_exec_run()` returns when its batch is full or its plan has finished,
and also once it has done a quantum of work since it was called, whether or
not the batch is full. The work is counted two ways, each up to
`RELIC_EXEC_QUANTUM`: the join's backtracks, and the tuples its scans and
lookups reject. Every turn of the join either descends, which it can do only
as many times as the plan has operations, or ends in a backtrack, so the
backtracks bound the turns; and a scan that rejects tuple after tuple within
one turn is bounded by the second count. A scan or lookup that uses up the
second count before it finds a match stops where it is, its cursor in its
state, and the executor records it as the operation to ask for its next
match; at a backtrack, it records the operation and resume flag it was about
to use. Either way it returns where a full batch would have left it, and the
caller resumes it exactly as it resumes a full batch. So a join that derives
nothing for hours -- a filter that rejects every binding, a scan that rejects
every tuple of a large relation, a scan whose duplicates all collapse --
returns to its caller as often as one that derives a batch every few steps.
Only an aggregate's fold cannot stop part way, since its running value is
not kept between calls; it charges the tuples it reads to the second count
and, once that is spent, finishes its walk and has the join pause as its
step returns -- at the next operation when the fold matched, and otherwise
at the fold, which has no next match -- so a call overruns its quantum by at
most one fold's walk: the group's chain where an index reaches it, and the
whole relation where the fold has no key or `RELIC_F_NO_INDEX` leaves it no
index. The quantum is a constant of the executor, a few thousand units,
small enough that a call takes well under a millisecond and large enough
that taking a batch, often empty, costs nothing measurable. The counts are
kept where they cost least: the backtracks in a register of the join's
loop, and the rejections in a local of the scan, given back as it returns.

**The caller that takes a batch looks after every return.** The serial
evaluator, after it takes a batch, reads the flag and fails with the
diagnostic of section 3 when it is raised, naming the relation the plan
derives into. It does so whether the plan returned paused or finished, so an
evaluation of many plans that each finish at once -- a recursion that
derives one tuple a round -- is stopped between plans as a long plan is
stopped within one.

**A worker abandons its job.** A worker of a shared round reads the flag
after each return of the executor and, when it is raised, abandons the job as
it abandons one whose buffer could not grow (`specs/design-parallel.md`
section 4.4): the other workers stop at their next claim, the candidates are
released, and the calling thread runs the round serially, where it meets the
interrupt at its first look and reports it. So an interrupt, like every other
failure of a shared round, is reported by the calling thread in a round run
serially, and a worker never calls caller code or writes a diagnostic.

**A gathered answer is delivered a row at a time.** Before each row of an
answer relation is passed to the callback, the flag is read, and a raised
flag ends the delivery with the diagnostic of section 3.

**What is not looked in.** Building an index, merging a round, seeding the
facts, folding an aggregate's groups, moving an index key's frontier up to a
delta, and discarding derived relations each take time bounded by what the
store holds rather than by how long the evaluation runs, and each is done
whole; a frontier, moreover, only moves forward, so its walks over an
evaluation together cover each chain once. Planning and the Magic rewrite are
bounded by the program and the rewrite's budgets. An interrupt made during
any of them is noticed at the executor's next return.

**Determinism.** An uninterrupted evaluation derives what it derives without
the quantum: the executor's state at a pause is the state it would have kept
across a full batch, the tuples it derived reach their relation in the order
it derived them, and between two calls the caller does only what it may do
between two batches. Relations, identifiers, answers, plans, diagnostics and
every counter are the same whatever the quantum, and the strategies stay
equivalent.

---

## 5. The interactive session

The tool interrupts only an interactive session that prompts
(`specs/design-cli.md` section 10.3), since only there is someone to return
to. While the session answers a line it catches `SIGINT`; the handler calls
`relic_interrupt()` on the session's context and writes a newline to the
terminal, so that the diagnostic does not begin on the line the terminal
echoed `^C` on. The handler restarts the system calls it interrupts, so a
write of the session's output or of a diagnostic is not cut short by the
signal.

The handler also records the interrupt for the line, which the session
clears as the line begins, for what the line does outside an evaluation. A
query is prepared before it runs, and the run forgets an interrupt made
before it began, so a line interrupted while its query was prepared fails
there instead of running. Under `-s` the tool writes the collected answer
itself, after the library has returned, and looks at the record before each
row, as the library does before each row it delivers. One window remains:
an interrupt that lands after the tool has looked at its record and before
the run has entered is forgotten by the run, and the query runs on until a
second Ctrl-C. Closing it would take an interrupt the library keeps across
operations, which section 2 declines for the sake of the interrupt made
while no operation runs; the window is the few instructions of a call.

An interrupt does not end a write. The handler restarts the write it
interrupts, so a session whose output goes to a pipe its reader has
stopped draining stays blocked in that write, or in the flush of its rows
before a diagnostic, and Ctrl-C does not return it to the prompt. Ending
the write would mean catching the signal without restarting system calls
and telling every write of the session that may be cut short apart from
those that may not, for a case where the reader, not the query, is what
stopped the session.

An interrupted line fails like any other: its diagnostic is reported, the
session goes on to the next line, and the exit status is 1, as for any line
that failed (`specs/design-cli.md` section 10.4). Rows written before the
interrupt stay written and end at a row boundary; the query's framing is not
completed -- json output lacks its closing bracket -- and `-v` does not count
it. A diagnostic reported while a query delivers rows follows the rows
delivered before it, since the tool writes out its buffered rows before it
writes a diagnostic.

While the session waits for a line, `SIGINT` has the disposition the session
started with, which by default ends the process with the terminal restored.
So does Ctrl-C during the loads and the `-q` queries before the session, and
throughout a run without `-i` or a session whose standard input or standard
error is not a terminal: a run with no one to return to ends, as it always
did.

---

## 6. Tests

* `tests/unit/test_interrupt.c`: an interrupt from an explain callback fails a
  materialization and a query with the diagnostic of section 3; the next
  query on the same context answers as one on a fresh context does; an
  interrupt made between operations is forgotten; an interrupt from a row
  callback fails a streaming and a gathered query after the rows already
  delivered; `relic_interrupt()` from a callback does not enter the
  context; `relic_interrupt(NULL)` does nothing; a row callback that both
  interrupts and stops the rows succeeds; a thread started as the evaluation
  begins interrupts a join that derives nothing.
* the same under `RELIC_F_PARALLEL_ALWAYS` with several threads, so that a
  worker's abandonment and the serial rerun's report are exercised.
* `tests/unit/test_exec.c`: a plan whose join derives nothing for more than a
  quantum returns paused with an empty batch, and running it to its end
  derives what an unpaused run derives; consecutive folds over groups larger
  than a quantum overrun it by at most one fold's walk; a fold that pauses
  the call has its value emitted by the next; a lookup whose chain rejects
  more than a quantum pauses inside the chain.
* `tests/terminal.sh`, at a pseudo-terminal: Ctrl-C during a query that
  would not end returns to the prompt, the next query answers, and the
  session exits with status 1.

---

## 7. What is deliberately absent

* **A step or time limit in the configuration.** `relic_config_t` is frozen
  (`specs/design-api.md` section 11). A caller that wants a deadline calls
  `relic_interrupt()` from a timer: a thread, or `alarm(3)` and a handler.
* **Interrupting a load or an import.** Their time is bounded by their input,
  and a load interrupted part way would add part of its facts, a state no
  other failure of a load leaves.
* **Resuming an interrupted evaluation.** The next evaluation discards what
  the interrupted one derived and starts again, as after any failure.
* **A return value that says the failure was an interrupt.** The convention
  of `specs/design-api.md` section 2 is -1 and a diagnostic; the caller that
  interrupted knows it did, and the diagnostic says so to a person.
* **Ctrl-C at the prompt.** A line editor reading the terminal does not
  reliably give up its read when a signal arrives: libedit retries the read
  once, then reports the end of input, and its own handler for a signal it
  does not end the process on can leave the terminal in the mode it reads
  in. So the prompt keeps the session's original disposition.
