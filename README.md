## Bob
						 (UNRELEASED)

Are you frustrated with build systems?

Here's Bob, he Builds.

He is Simple - It's For the C Programmer - It's just Bob.

```elf

options := {
	workers = 4
	verbosity = 0
}

compile := {
	name = "compile"
	command_line = "clang-cl /c main.c /Fobuild\\main.obj"
	inputs = {"main.c"}
	outputs = {"build\\main.obj"}
}

link := {
	name = "link"
	command_line = "clang-cl build\\main.obj /Febuild\\hello.exe"
	inputs = {"build\\main.obj"}
	outputs = {"build\\hello.exe"}
	dependencies = {compile}
}

tasks := {compile, link}

ret {
	options = options
	tasks = tasks
}

```

It works by running an 'elf' script. Looks for 'build.elf' by default.

The script returns a table with tasks, and options.

Then Bob builds it.


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

All the script does is return a table of tasks.

Generate the table however you want.

Bob, takes the table, loads the tasks, builds a graph and, builds it.

That's [clap] Freaking [clap] It [clap]

It's just Bob.
