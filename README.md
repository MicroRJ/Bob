## Bob, the Builder
						 (UNRELEASED)

Frustrated With Build Systems? Here's Bob, the Builder.

It works by running an 'elf' script, which is a real programming language
and not a crappy DSL.

If none is given it looks for build.elf.

The script returns a table of tasks.

Then Bob does the rest.

It's Simple - It's For C - It's Bob, the Builder.

```elf

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

ret {
	tasks = {compile, link}
}

```

I've used some build systems in the past, from scripts, to large production
tools. Now granted, am not all too familiar with them, mostly from the outset
I despised them.

They all have problems, and little to no redeeming qualities.

A) They use an ugly, crappy, inconsistent DSL with no power.
B) They are bloated, hard to understand and obscure.
C) If it's a build script, I have to do everything myself, and what C developer wants to use Python.

Bob, is Simple.

It has a normal, C like programming freaking language.
Similar in spirit to Lua but actually straightforward.

All the script does is return a table of tasks.
You can generate the table however you want.

Bob, takes the table, loads the tasks, builds a graph and executes it.

That's ... Freaking ... It ...

It's Bob, the Builder.