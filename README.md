# qmlcoverage

> Superseded: the in-tree version, with tests and documentation, is branch `coverage-service` of
> [igouss/qtdeclarative-coverage](https://github.com/igouss/qtdeclarative-coverage/tree/coverage-service).
> This standalone prototype builds against an installed Qt and speaks an older wire format.

Line and function coverage for QML and JavaScript, written as lcov. Prototype for
[QTBUG-151755](https://qt-project.atlassian.net/browse/QTBUG-151755), continuing the approach Ulf
Hermann sketched in 2015 (Gerrit 122288/122429): coverage as a QML debug service.

```
cmake -S . -B build -G Ninja && cmake --build build
build/qmlcoverage --plugins build/plugins -o coverage.info qmltestrunner -input tests/
genhtml -o coverage-html coverage.info
```

`qmlcoverage` starts the program with `-qmljsdebugger=file:<socket>,block,services:CodeCoverage`,
sums the reports of every QML engine per file and writes lcov. It exits with the program's exit
code. It fails instead of writing an empty report when nothing arrived (exit 4: plugin not found
or built for another Qt), when the program crashed (3), or when an engine did not answer the
final snapshot (5).

**Programs that run until stopped** (a shell, a bar): press Ctrl+C or send `qmlcoverage` SIGTERM.
It asks every engine for a snapshot, stops the program and writes the report. The program runs in
its own process group, so the terminal's Ctrl+C reaches only `qmlcoverage`.

**qrc: files** are read by the service inside the program and written below `<output>.src/`
(or `--sources dir`); their lcov records point there.

## How it works

`plugin/` is a `qmltooling` plugin, `CodeCoverage`. For each engine it installs a
`QV4::Debugging::Debugger` that never pauses. Attaching any debugger puts the engine in debug
mode, and every executed line then passes through the hook:

- the compiler emits a `Debug` instruction at each line change (`qv4bytecodegenerator.cpp`)
- the JIT is skipped while a debugger is attached (`VME::exec`)
- the disk cache is off, which rejects compiled-in AOT units, so they load from source
  (`ExecutionEngine::diskCacheOptions`); `tests/check.sh` checks that AOT-compiled
  `QtTest/TestCase.qml` reports hits

Executable lines come from each compiled function's line table, so loaded but unexecuted code
reports zeros. Reports are deltas: an engine sends its hits since its previous report when it is
removed and when the client asks for a snapshot; the client sums them.

The plugin uses Qt's private engine API, so it refuses to run (with a warning) in any Qt other
than the one it was built against.

## Limits

Inherent to an out-of-tree service on Qt 6.12. Each needs an engine change, listed for upstream:

- **No branch coverage.** The hook only sees line changes, so two outcomes on one line
  (`a ? b : c`) are indistinguishable. Upstream: in debug mode, also emit `Debug` at jump targets,
  and report per-jump outcomes.
- **A `for` header line counts one entry**, however often its test runs: the update is emitted on
  the body's line and the jump back lands after the header's `Debug`. Coverage is right, the
  count is not. Upstream: give the update and test their own source location.
- **Closing-punctuation lines are not reported.** The epilogue sits on a function's closing brace
  and is dead code after a final `return`, so `}` would read as uncovered. The client drops lines
  holding only `}`, `)`, `]`, `;`, `,`, like statement-based JS coverage tools.
- **Slower runs.** Everything runs in the interpreter and every line change calls the hook:
  measured ×1.01–×1.23 wall time on five of qtdeclarative's `tests/auto/qmltest` suites (mostly
  waiting on events), ×5.8 on a tight JS loop. All 28 of those suites give identical pass/fail
  totals with and without coverage.
- **One debugger per engine**: cannot run together with the QML debugger or profiler services
  that install a debugger.
- **Main-thread engines only**: Qt registers no other engine with the debug server
  (`QJSEnginePrivate::addToDebugServer`), so code in a `QQmlEngine` on a worker thread is not
  covered.

## Tests

- `ctest --test-dir build` runs `tests/protocol` (reports decode; a corrupt or foreign report,
  such as one from Qt dev's in-tree service, is refused without allocating its claimed length)
  and `tests/check.sh`: exact lcov records for the fixtures (zero, one,
  many calls; loops, branches, switch, try/catch/finally; an imported `.js` library; two engines;
  qrc sources; Ctrl+C on a program that never exits, on two live engines, and after every engine
  is gone; a file with no readable source skipped), failing loudly on a missing plugin, a plugin
  built for an older or newer Qt, and an unwritable report or source copy, and
  `lcov --summary` as the format gate.
- `tests/mutate.py <workdir> <sources...>` runs universalmutator mutants of the C++ sources against
  `check.sh` in parallel; it first proves the check does not fail under that load.
