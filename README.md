###    Are you frustrated with your build system?

                    Bob, Builds.

			     - For the C Programmer -

			     		 (UNRELEASED)

```elf

options := {
	workers   = 4
	verbosity = 0
}

compile := {
	name          = "compile",
	command_line  = "clang-cl /c main.c /Fobuild\\main.obj",
	inputs        = {"main.c"},
	outputs       = {"build\\main.obj"},
}

link := {
	name          = "link",
	command_line  = "clang-cl build\\main.obj /Febuild\\hello.exe",
	inputs        = {"build\\main.obj"},
	outputs       = {"build\\hello.exe"},
	dependencies  = {compile},
}

targets := {link}

ret {
	options = options
	targets = targets
}

```

It works by running an 'elf' script ('build.elf' by default).

The script returns a table with targets, and options.

Then Bob builds it.

## Building Bob

```bat
build.bat
```

## Rant

I despise build systems, especially the "good" ones.

They all have problems, and little to no redeeming qualities.

A) They use an ugly, crappy, inconsistent DSL with no power.
B) They are bloated, hard to understand and obscure.
C) If it's just some random script, I have to do everything myself, and what C developer
wants to use Python.

Bob is Simple.

It uses a normal, minimal, general purpose, C like programming
freaking language.

All the script does is return a table of targets.

Generate the table however you want.

Bob takes the targets, discovers their dependencies, builds a graph and builds it.

That's [clap] Freaking [clap] It [clap]

It's just Bob.
