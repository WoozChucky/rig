---
layout: home

hero:
  name: Rig
  text: Small, explicit, native
  tagline: A compiled systems programming language for Windows and Linux, in the spirit of Jai.
  image:
    light: /logo.svg
    dark: /logo-dark.svg
    alt: An anvil
  actions:
    - theme: brand
      text: Install
      link: /install
    - theme: alt
      text: Read the specification
      link: /spec/
    - theme: alt
      text: GitHub
      link: https://github.com/WoozChucky/rig

features:
  - title: Native executables
    details: Compiled ahead of time through LLVM to x86-64 executables for Windows and Linux, linked with the platform's own tools and C runtime. No garbage collector, no virtual machine.
    link: /spec/implementation
    linkText: How it is compiled
  - title: C layout, C calls
    details: Structs are laid out as C lays them out, and a C function declared once with <code>#foreign libc</code> is called like any procedure, with the platform's C calling convention.
    link: /spec/procedures#76-foreign-procedures
    linkText: Foreign procedures
  - title: Checked by default
    details: A division by zero, a cast that would change a number, a null dereference and an index out of bounds stop the program with a message. <code>#no_abc</code> turns bounds checks off where you choose.
    link: /spec/runtime-checks
    linkText: Runtime checks
  - title: No undefined behaviour
    details: Integers wrap, a shift amount wraps to its type's width, a float converted to an integer saturates and variables start zeroed. What the language defines, it defines.
    link: /spec/introduction#12-goals
    linkText: Goals
  - title: Precise diagnostics
    details: Every compile-time error names its file, line and column and shows the line. Syntax errors and type errors are reported together.
    link: /spec/compile-time-errors
    linkText: Compile-time errors
  - title: Debug information everywhere
    details: Every build carries it, a PDB on Windows and DWARF 5 on Linux, and keeps frame pointers, so Visual Studio, WinDbg and gdb show procedures, lines and the call stack.
    link: /spec/implementation#107-debug-information
    linkText: Debug information
---

## A taste of Rig

<!--@include: ./.vitepress/generated/taste.md-->
