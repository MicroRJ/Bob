## Bob The Builder
						 (UNRELEASED)

Frustrated With Build Systems? Here's Bob, the builder.

It works by running an 'elf' script. If none is given
it looks for build.elf.

The script returns a table of tasks.

Then bob does the rest.

It's Simple - It's For C - I Like It - It's Bob, The Builder.

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
