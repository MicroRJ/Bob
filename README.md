### Bob

     Are you frustrated with your build system?

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
	command_line  = "clang-cl /c main.c /Fomain.obj",
	inputs        = {"main.c"},
	outputs       = {"main.obj"},
}

link := {
	name          = "link",
	command_line  = "clang-cl main.obj /Fehello.exe",
	inputs        = {"build\\main.obj"},
	outputs       = {"build\\hello.exe"},
	dependencies  = {compile},
}

entries := {}

entries.build = fun() {
	// 'bob' is just a table injected by bob, which has the build function
	success := bob.build({ options = options, targets = {link} })
	ret success
}
ret entries

```

It works by running an 'elf' script ('build.elf' by default).
The script returns a table of functions, which you can invoke
from the command line.

```cmd
bob
bob build
bob pet
bob do_something
bob do_anything_really
```

'bob' alone invokes 'build' by default.

```elf
entries.clean = fun() {
	// run arbitrary elf code to clean my directory
}

entries.pet = fun() {
	elf.printl("<3 <3 <3 <3")
}

entries.rant_about_how_much_i_hate_build_systems = fun() {
	elf.printl("See the rant section below")
}
```


Please see the root build.elf to learn more


## Building Bob

```bat
build.bat
```

## Rant

I despise build systems, especially the "good" ones.

They all have problems, and little to no redeeming qualities.

A) They use ugly, crappy, inconsistent DSL's with no power.

B) They are bloated, hard to understand and obscure.

C) If it's just some random script, I have to do everything myself, and I don't like any of
	the scripting languages out there.

Bob is Simple.

It uses a normal, minimal, general purpose, C-Like programming
freaking language.

All the script does is call 'bob.build()' with a table of targets.

Generate the table however you want.

Bob takes the targets, discovers their dependencies, builds a graph and builds it.

That's [clap] Freaking [clap] It [clap]

It's just Bob.
