# Bob

> Bob, Builds.

Bob is a small programmable build system for C.

Build scripts are ordinary [elf](https://github.com/MicroRJ/elf) programs. They
generate a graph of tasks, and Bob executes that graph in parallel, tracks
compiler-discovered dependencies, and explains why something rebuilt.

The graph executor is also a C library. The elf frontend is one way to drive
Bob; it is not the architecture Bob is trapped inside.

Bob is currently in early development. The supported host is Windows x64, and
the public API and build-state format may still change.

## A build

```elf
elf.fs.create_directory("build")

compile := {
	name = "compile main",
	command_line = "clang-cl /nologo /c main.c /Fobuild\\main.obj",
	inputs = { "main.c" },
	outputs = { "build/main.obj" },
}

link := {
	name = "link hello",
	command_line = "clang-cl /nologo build\\main.obj /Febuild\\hello.exe",
	inputs = { "build/main.obj" },
	outputs = { "build/hello.exe" },
	dependencies = { compile },
}

entries := {}

entries.build = fun() {
	ret bob.build({
		targets = { link },
		options = { workers = 4 },
	})
}

ret entries
```

Save that as `build.elf`, then run Bob:

```text
> bob
[1/2 succeeded] compile main
[2/2 succeeded] link hello

> bob
[1/2 up-to-date] compile main
[2/2 up-to-date] link hello
```

Ask Bob why it made the current incremental decision:

```text
> bob --explain
[explain] compile main: inputs are not newer than outputs
[1/2 up-to-date] compile main
[explain] link hello: inputs are not newer than outputs
[2/2 up-to-date] link hello
```

`bob` runs the `build` entry by default. A script can expose any entries it
wants:

```text
bob
bob build
bob run
bob clean
bob rant_about_how_much_i_hate_build_systems
```

An entry is just an elf function. It can construct a graph, generate files,
call other scripts, remove a directory, print something stupid, or do whatever
else the build needs.

## Why Bob?

- **The build is a program.** Use functions, loops, tables, closures, string
  interpolation, and modules instead of fighting a deliberately weak DSL.
- **Tasks are plain data.** Generate them, transform them, combine subprojects,
  and pass the resulting graph to `bob.build()`.
- **Dependencies create parallelism.** Bob schedules ready tasks across worker
  threads while respecting the graph.
- **The compiler tells Bob about headers.** Bob consumes compiler-generated
  dependency files instead of trying to understand C with an include scanner.
- **Incremental decisions include configuration.** BLAKE3 fingerprints cover
  commands, working directories, normalized paths, and relevant task flags.
- **Incremental progress survives interruption.** Completed task state is
  appended as the build runs and compacted into a checksummed binary snapshot.
- **Rebuilds are explainable.** `--explain` reports why a task ran or remained
  up to date.
- **The executor is a library.** C programs can construct a generic Bob graph,
  attach callbacks to nodes, and execute it without using the build frontend.

## The model

A build task has:

- a command line;
- zero or more inputs;
- zero or more outputs;
- zero or more task dependencies;
- an optional working directory;
- optional metadata such as include directories and transparency.

Tasks form a directed acyclic graph. A task becomes ready after its dependencies
finish. Bob runs ready tasks concurrently, blocks dependents when a dependency
fails, and finishes when every reachable task is terminal.

For incremental builds, Bob normalizes and interns paths, records dependencies
reported by the compiler, fingerprints the task description, and stores the
result under `.bob/state` in the build root.

That is the core of it. Generate the task table however you want, then give it
to Bob.

## Getting started

Requirements:

- Windows x64
- Visual Studio C++ build tools
- `clang-cl` available from the command line
- Git with submodule support

Clone Bob and its dependencies:

```bat
git clone --recursive https://github.com/MicroRJ/Bob.git
cd Bob
```

Build Bob using its checked-in blessed executable:

```bat
build.bat
```

The new executable is written to `build\bob.exe`.

Try the included example:

```bat
cd example
..\build\bob.exe
..\build\bob.exe run
..\build\bob.exe --explain
```

The example's generated files live under `example\build`, and its persistent
incremental state lives under `example\.bob`.

## Command line

```text
bob [build-file] [entry] [options]
```

Bob uses `build.elf` and the `build` entry when neither is specified.

```text
-q, --quiet          Suppress task output.
--explain            Explain incremental decisions.
--verbose [N]        Control Bob's internal diagnostic verbosity.
--workers N          Set the worker count.
--profile            Write profiling information.
--profile-threads    Include worker-thread profiling.
--version            Print Bob and elf versions.
```

Verbosity controls Bob's own diagnostics. Task output is printed by default and
is controlled separately by `--quiet`.

## As a C library

The abstract heart of Bob is independent of command lines and files. A `Bob`
contains nodes and dependency edges. Each node has a callback, user data, and a
result. `bob_execute()` runs the graph using the requested worker count and can
report start and completion events to the host.

The build layer sits on top of that executor and adds processes, inputs,
outputs, fingerprints, compiler dependencies, and persistent state.

This separation is intentional. Bob can be used as a build-system runner, as a
graph execution library embedded in another C program, or through a different
frontend in the future.

## Current limitations

- Only the Windows platform implementation is complete.
- Linux support is planned but not yet implemented.
- MSVC dependency handling is not complete; Clang, clang-cl, and GCC-style Make
  dependency files are the mature path.
- There is no remote or content-addressed build cache yet.
- Tasks do not yet have isolated environment overrides.
- The C API and binary state format are still allowed to evolve.

## Philosophy, or: the rant

I despise build systems, especially the "good" ones.

They tend to have at least one of these problems:

1. They use an ugly, inconsistent DSL with no real power.
2. They are bloated, obscure, and difficult to understand.
3. They are merely scripts, so I have to implement the entire build system
   myself in a scripting language I do not particularly like.

Bob uses a normal, minimal, general-purpose, C-like programming language. The
script calls `bob.build()` with a table of targets. Bob takes those targets,
constructs the graph, tracks what changed, and builds it.

Generate the table however you want.

That's freaking it.

It's just Bob.
