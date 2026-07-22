# platform

Small operating-system primitives shared by C projects.

The first pass contains virtual memory, time, sleeping, handle-based file access,
processes, threads, mutexes, and condition variables for Windows. The public API
uses null-terminated strings and has no project dependencies.

This is a prototype. Bob still uses its existing platform layer while the API
settles.

Build the library or run the tests with Bob:

```text
bob build.elf build
bob build.elf test
```
