# relic by example

A tour of relic through small programs you can run. Every program and table
shown here lives in `tests/examples`, and every command is run by
`make check` against the output shown, so what you read is what relic
does. Copy that directory somewhere to play in -- some examples write
files -- and follow along there.

`relic(1)` is the reference for the language and every option; this file is
about putting them to work.

* [A family tree](#a-family-tree) -- facts, rules, recursion, negation, counting
* [What can I work on next?](#what-can-i-work-on-next) -- negation as a planning tool
* [Fewest stops on the metro](#fewest-stops-on-the-metro) -- arithmetic and `min`
* [Taking a source tree apart](#taking-a-source-tree-apart) -- strings, regular expressions, `sum` and `max`
* [Data in: importing tables](#data-in-importing-tables) -- `-I` and `-T`
* [Data out: exporting relations](#data-out-exporting-relations) -- `-O`
* [Exploring interactively](#exploring-interactively) -- `-i`
* [When something is wrong](#when-something-is-wrong) -- `-c`, warnings, `-v`, `-e`
* [Big inputs](#big-inputs) -- `-a`, `-j`, `-l`
* [Embedding relic in C](#embedding-relic-in-c)

---

## A family tree

A program is facts, which say what is true, and rules, which say what
follows. Here are four generations of one family, and the relations between
them:

```text family.dl
% Four generations of one family, and who is related to whom.
parent(alice, bob).
parent(alice, carol).
parent(bob, dave).
parent(bob, erin).
parent(carol, frank).
parent(dave, gina).

ancestor(X, Y) :- parent(X, Y).
ancestor(X, Z) :- parent(X, Y), ancestor(Y, Z).
sibling(X, Y) :- parent(P, X), parent(P, Y), X != Y.
cousin(X, Y) :- parent(A, X), parent(B, Y), sibling(A, B).
person(X) :- parent(X, _).
person(Y) :- parent(_, Y).
childless(X) :- person(X), not parent(X, _).
descendants(X, N) :- person(X), N = count { ancestor(X, D) }.
```

`ancestor` is recursive: a parent is an ancestor, and so is a parent's
ancestor. relic computes it to a fixed point, however deep the tree. Ask for
everyone descended from alice with `-q`; `-s` sorts the answer:

```sh
$ relic -s -q 'ancestor(alice, X)?' family.dl
bob
carol
dave
erin
frank
gina
```

Ask several questions at once. Each answer is separated from the next by an
empty line. `childless` uses `not`: a person with no child at all, where `_`
under `not` is a column the test does not care about.

```sh
$ relic -s -q 'cousin(dave, X)?' -q 'childless(X)?' family.dl
frank

erin
frank
gina
```

`count { ... }` folds a body into a number. Here it counts each person's
descendants, written as CSV for a spreadsheet:

```sh
$ relic -s -F csv -q 'descendants(X, N)?' family.dl
alice,6
bob,3
carol,1
dave,1
erin,0
frank,0
gina,0
```

`-F` also takes `tsv`, `json` and the default, `text`, and `-o` writes the
answer to a file instead of the standard output.

## What can I work on next?

Negation turns a list of dependencies into a to-do list. This pipeline says
which steps wait on which, and which are finished:

```text release.dl
% A release pipeline: which steps wait on which, and which are finished.
step(fetch).
step(compile).
step(test).
step(package).
step(docs).
step(release).
waits(compile, fetch).
waits(test, compile).
waits(package, compile).
waits(docs, fetch).
waits(release, test).
waits(release, package).
waits(release, docs).
done(fetch).
done(compile).

before(S, B) :- waits(S, B).
before(S, B) :- waits(S, M), before(M, B).
blocked(S) :- waits(S, B), not done(B).
ready(S) :- step(S), not done(S), not blocked(S).
cycle(S) :- before(S, S).
remaining(S, N) :- step(S), N = count { before(S, B), not done(B) }.
```

A step is ready when it is not done and nothing it waits on is unfinished:

```sh
$ relic -s -q 'ready(S)?' release.dl
docs
package
test
```

How far is each step from being unblocked? Count what still has to happen
before it:

```sh
$ relic -s -q 'remaining(S, N)?' release.dl
compile 0
docs 0
fetch 0
package 0
release 3
test 0
```

A dependency cycle would deadlock the pipeline. `-v` counts each query's
answers on the standard error, so an empty answer is something you see
rather than an absence you have to notice:

```sh
$ relic -v -q 'cycle(S)?' release.dl
query 1: 0 rows
```

A query with no variables is a yes-or-no question: it writes one empty row
when it holds and nothing when it does not, so `-v` makes the answer easy
to read. Is the release blocked?

```sh
$ relic -v -q 'blocked(release)?' release.dl

query 1: 1 row
```

In a script, look for that empty line with `grep`, since `$(...)` would
strip it:

```sh
$ relic -q 'blocked(release)?' release.dl | grep -q '^$' && echo blocked
blocked
```

This works for one query at a time: between the answers of several, relic
writes an empty line of its own.

## Fewest stops on the metro

Comparisons and integer arithmetic go in a rule's body. This map of a small
metro computes how many stops apart two stations are:

```text metro.dl
% A toy metro map.  Each link runs both ways.
link(central, museum).
link(museum, park).
link(park, zoo).
link(central, harbor).
link(harbor, market).
link(market, zoo).
link(market, stadium).

next(X, Y) :- link(X, Y).
next(X, Y) :- link(Y, X).
hops(X, Y, 1) :- next(X, Y).
hops(X, Z, N) :- hops(X, Y, M), next(Y, Z), X != Z, M < 6, N = M + 1.
stops(X, Y, N) :- hops(X, Y, _), N = min(M) { hops(X, Y, M) }.
```

`hops` counts the stops of every route, and `M < 6` stops it from walking in
circles forever: a rule that builds a new number from one it derived must be
bounded. `min(M) { ... }` then keeps the shortest route between each pair.

```sh
$ relic -q 'stops(central, zoo, N)?' metro.dl
3
```

As JSON, ready for a web page. An integer is a JSON number and a symbol a
plain JSON string; a string value is a JSON string holding its quoted
literal, as `"\"docs\""`, so every type survives the trip:

```sh
$ relic -s -F json -q 'stops(central, X, N)?' metro.dl
[
  ["harbor",1],
  ["market",2],
  ["museum",1],
  ["park",2],
  ["stadium",3],
  ["zoo",3]
]
```

## Taking a source tree apart

Strings come with six functions -- `length`, `cat`, `substr`, `index`,
`match` and `str` -- and `match` takes a regular expression. Together with
the aggregates `sum` and `max` they answer the questions a build tool asks:

```text tree.dl
% A source tree, and what a build makes of it.
file("src/main.c", 1200).
file("src/util.c", 300).
file("src/util.h", 80).
file("tests/test_util.c", 450).
file("docs/README.md", 900).

object(S, O) :- file(S, _), substr(S, length(S) - 2) = ".c",
    O = cat(substr(S, 0, length(S) - 2), ".o").
test_file(F) :- file(F, _), match(F, "^tests/.*\\.c$") >= 0.
dir_of(F, D) :- file(F, _), D = substr(F, 0, index(F, "/")).
dir_size(D, S) :- dir_of(_, D), S = sum(Size) { dir_of(F, D), file(F, Size) }.
largest(F) :- file(F, Size), Size = max(N) { file(_, N) }.
label(F, L) :- file(F, Size), L = cat(F, cat(": ", str(Size))).
```

Which object file does each C source become, and which sources are tests?

```sh
$ relic -s -q 'object(S, O)?' -q 'test_file(F)?' tree.dl
"src/main.c" "src/main.o"
"src/util.c" "src/util.o"
"tests/test_util.c" "tests/test_util.o"

"tests/test_util.c"
```

How big is each directory, and what is the largest file?

```sh
$ relic -s -q 'dir_size(D, S)?' -q 'largest(F)?' tree.dl
"docs" 900
"src" 1580
"tests" 450

"src/main.c"
```

Strings print in quotes and symbols bare, because they are different values:
the symbol `docs` and the string `"docs"` never join. `str` turns any value
into a string, so a number can be part of a label:

```sh
$ relic -q 'label("src/util.h", L)?' tree.dl
"src/util.h: 80"
```

## Data in: importing tables

Real data arrives as tables. `-I` reads a CSV or TSV file as facts, and `-T`
says the type of each column, once per predicate. Nothing about a column is
guessed. Here are some game scores:

```text score.tsv
ada	tetris	4200
ada	snake	310
bo	tetris	5100
bo	pong	12
cy	snake	455
cy	tetris	3900
```

and a leaderboard over them:

```text board.dl
% A leaderboard over game scores imported from a table.
best(G, P, S) :- score(P, G, S), S = max(T) { score(_, G, T) }.
total(P, T) :- score(P, _, _), T = sum(S) { score(P, G, S) }.
```

`-I score.tsv` names the predicate by the file name and the format by the
extension. Who holds each game's high score, and who has the most points?

```sh
$ relic -s -T score:symbol,symbol,integer -I score.tsv -q 'best(G, P, S)?' -q 'total(P, T)?' board.dl
pong bo 12
snake cy 455
tetris bo 5100

ada 4510
bo 5112
cy 4355
```

A table can come from the standard input, which is `-`. A head,
`format:predicate=path`, names what such a path cannot:

```sh
$ relic -T score:symbol,symbol,integer -I tsv:score=- -q 'best(tetris, P, S)?' board.dl < score.tsv
bo 5100
```

`_` in `-T` skips a column. A query can filter a table with no rules at
all. With no file operand relic reads its program from the standard input,
so name `/dev/null`, an empty program:

```sh
$ relic -T score:symbol,_,integer -I score.tsv -q 'score(P, S), S > 1000?' /dev/null
ada 4200
bo 5100
cy 3900
```

Rules and data need not share a file. This program holds a rule and its own
query, which relic answers when no `-q` is given:

```text rules.dl
% The people old enough to vote.
adult(P) :- person(P, Age), Age >= 18.
adult(P)?
```

Its facts can come from another program file -- every file named is one
program -- or from a spreadsheet. A CSV export usually has a header line,
which `,header` in the head skips:

```text people.csv
name,email,age
ann,ann@example.org,34
bob,bob@example.org,12
cid,cid@example.org,51
```

```sh
$ relic -T person:symbol,_,integer -I csv,header:person=people.csv rules.dl
ann
cid
```

A symbol is a name: letters, digits and underscores, beginning with a
lowercase letter. Any other text, such as an e-mail address, must be read
as a `string`, and relic refuses the table otherwise. Strings are also what
the string functions take apart; on a symbol they give no value, unless
`str` converts it first.

```sh
$ relic -T person:symbol,string,integer -I csv,header:person=people.csv -q 'person(P, E, A), A > 30?' /dev/null
ann "ann@example.org" 34
cid "cid@example.org" 51
```

## Data out: exporting relations

`-O` writes a whole relation to a file once the queries have run. Like
`-I`, a path alone names the predicate by its file name and the format by
its extension:

```sh
$ relic -s -O ancestor.tsv family.dl && cat ancestor.tsv
alice	bob
alice	carol
alice	dave
alice	erin
alice	frank
alice	gina
bob	dave
bob	erin
bob	gina
carol	frank
dave	gina
```

A `.dl` export writes facts in relic's own syntax, so a later run loads the
result as a program, every value keeping its type:

```sh
$ relic -O ancestor.dl family.dl && relic -s -q 'ancestor(X, gina)?' ancestor.dl
alice
bob
dave
```

A head names the format and predicate when the file name cannot, and
several exports share one run. With `-a` the whole program is evaluated
first and every export written from it, and `-v` counts what each export
wrote:

```sh
$ relic -a -v -O stops.csv -O json:next=map.json metro.dl
export 1: 42 rows
export 2: 14 rows
```

When a name has more than one arity -- `edge/2` and `edge/3` are different
predicates -- a head picks one, as `-O tsv:edge/3=edge3.tsv`.

An export creates its file or truncates it, as `-o` does, so point it only
at a file you mean to replace. relic refuses an export to a path the
command line spells for something else, so `-O family.dl family.dl` is a
usage error. But it compares paths as written: `-O dl:ancestor=./family.dl
family.dl` names the same file another way, is not caught, and replaces
your program with its facts.

## Exploring interactively

`-i` answers queries typed one per line after the program loads. At a
terminal it prompts, and edits and recalls lines where relic was built with
libedit. Commands begin with a dot:

```sh
$ relic -i metro.dl
relic> .help
.help         list the commands
.plan QUERY   run QUERY and show its plan instead of its rows
.quit         end the session
.stats        show the evaluation counters
relic> stops(central, X, 2)?
park
market
relic> .quit
```

`.plan QUERY` shows how a query runs, as `-e` below does, and `.stats` the
work done so far. In a session that prompts, Control-C while a query runs
stops that query and the session goes on. While the session waits for a
line, or when it does not prompt, Control-C ends relic.

## When something is wrong

`-c` checks a program without running it, and reports every mistake at its
line and column:

```text broken.dl
% Two mistakes: a variable only a negation binds, and a misspelled name.
parent(alice, bob).
grandparent(X, Z) :- parent(X, Y), parnet(Y, Z).
orphan(X) :- not parent(_, X).
```

```sh
$ relic -c broken.dl
broken.dl:4:8: error: variable X in rule orphan/1 is not bound by a positive body literal or an assignment
broken.dl:3:36: warning: predicate parnet/2 is referenced but never defined
```

Errors make relic exit 1, and warnings alone leave the status 0, so `-c`
fits in a pre-commit hook. A malformed command line exits 2.

relic never converts one type to another. Values of different types are
never equal, and `<`, `<=`, `>` and `>=` between them are false. When data
comes from two sources, that can hide a mistake, and `-vv` points at the
first such ordering comparison in each rule:

```text mixed.dl
% Ages arrive from two sources; one wrote them as strings.
age(ann, 34).
age(bob, "12").
adult(P) :- age(P, A), A >= 18.
```

```sh
$ relic -vv -q 'adult(P)?' mixed.dl
mixed.dl:4:24: note: ordering comparison between a string and an integer is false
ann
query 1: 1 row
```

`-n` runs the queries and writes none of their rows, so with `-v` it counts
answers you do not want to see:

```sh
$ relic -n -v -q 'hops(X, Y, N)?' metro.dl
query 1: 112 rows
```

`-e` shows how each query is evaluated: the order of its joins, the indexes
it uses, and the Magic Sets rewrite that keeps a query from deriving more
than its constants need. Asked whether `release` is blocked, relic seeds
the rewrite with `release` and looks only at what that step waits on,
never at the other steps:

```sh
$ relic -n -e -q 'blocked(release)?' release.dl
magic rewrite of $q0 :- blocked(release).
    adorn blocked/1 b
    seed magic$blocked$b(release).
    stratum 0, component 0: magic$blocked$b/1
        facts: 1
    stratum 1, component 1: blocked$b/1
        rule blocked$b(S) :- magic$blocked$b(S), waits(S, B), not done(B).
            scan magic$blocked$b(S)
            lookup waits(S, B) key={0}
            anti-member done(B)
            emit blocked$b(S)
query $q0 :- blocked$b(release).
    member blocked$b(release)
    stream $q0
```

When a program is slower than it should be, `-X stats` writes the counters
of the run to the standard error: iterations, derivations, tuples scanned,
looked up and held, and bytes of storage. Like every `-X` switch it is a
debugging aid rather than a stable interface.

## Big inputs

A query normally derives only what its constants can reach. `-a` evaluates
the whole program first and answers every query from it, which pays when
one run asks many questions of the same relations.

`-j` shares evaluation among threads, and `-j 0` uses one per processor. A
round of evaluation is shared only when it holds enough work to repay the
threads, so a program as small as this one runs on one thread anyway. The
answers, their order and every diagnostic are the same whatever the count:

```sh
$ relic -j 4 -q 'stops(central, zoo, N)?' metro.dl
3
```

A program can derive a lot. `-l` bounds the tuples all relations hold and
the memory evaluation uses for them, their indexes and their values, and a
query that would exceed either fails with an error and writes no rows:

```sh
$ relic -l tuples=20 -q 'stops(central, zoo, N)?' metro.dl
relic: error: limit exceeded: tuples=20 while deriving next/2
```

## Embedding relic in C

Everything above is a client of `librelic`: load a program, prepare a
query, and receive its rows through a callback, with the same limits,
threads and diagnostics. `relic(3)` documents the interface, and
`examples/query.c` is a complete program that answers a query in under a
hundred lines. Build against the installed library with
`pkg-config --cflags --libs librelic`.
