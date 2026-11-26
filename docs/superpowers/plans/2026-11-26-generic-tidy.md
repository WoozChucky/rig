# Tidy what generic procedures left: plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Do what issue #243 asks, the tidying generic procedures (#226, PR #242) left: one walk over a generic's names in `check_generic.cpp`, and a message for an array literal written as C writes one.

**Architecture:** One file-level walk, `walk_names`, goes through an expression or a written type, with a `switch` that has a case for every `Ast_Kind` and no `default` (as `ast.cpp`'s `ast_clone`), so `-Wswitch` flags a new kind at compile time. At each name it calls the caller's action, and tells it where the name is (`Name_Place`: inside an array's count, a pointer's target). The six near-copy walks become actions. In the parser, a `[` where an expression starts is checked by a token look-ahead (in the style of `proc_type_follows`) before it is read as a type.

**Tech Stack:** C-style C++23, no STL, pinned clang 23.1.5 (`build.bat`, `build.sh`).

**Spec:** issue #243 (`gh issue view 243`); `docs/rig-spec.md` 5.17 (array literals), 5.20 (types where a value goes), A.4 (the paragraph under the expression grammar). Spec 7.11 stays as it is.

## Global Constraints

- Task 1 changes no behaviour: no message, position, order or exit code changes, and no expected file (`tests/run`, `tests/errors`, `tests/debug`) changes. If one does, that is a bug to find. Do not regenerate the file.
- Task 2 changes only the message for a `[` that starts an expression and is read as a C-style literal (the rule is in Task 2). Every other parser message stays the same, including where a type goes (`g : [1, 2]int;`, `#type [1, 2]`).
- Code is study material: each new or merged piece gets a comment in the style around it saying what it does and why. Comments that name a removed function (`check_count_names`, `check_count_type_names`, `names_type`, ...) are fixed in the same commit, and so is the file's header comment.
- Unity builds: no new source file. File-level `static` names must stay unique across `src/all.cpp` (`walk_names`, `Name_Place`, `starts_a_type` and `c_style_literal_follows` are unused today).
- `-Wall -Wextra`, no warnings, debug and release, Windows and Linux.
- Every commit must pass on both hosts: `bin\unit-tests.exe` and `bin\run-tests.exe`. `run-tests` runs the debugger tests last (cdb on Windows, gdb on Linux), and they must PASS, not SKIP.
  - Windows (PowerShell, `C:\dev\rig-wt\243`): `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c build.bat`, then `.\bin\unit-tests.exe` and `.\bin\run-tests.exe`.
  - Linux (WSL, `~/rig-wt/243`, never through `/mnt/c`), from Git Bash after the push:
    ```bash
    MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
    cd ~/rig-wt/243 && git pull -q --ff-only
    RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
    EOF
    ```
    The first time, run `git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/243 issue/243-generic-tidy`.
  - Before the final review, run `build.bat release` and `./build.sh release`, each followed by both test programs.
- Workflow:
  - Branch `issue/243-generic-tidy` in worktree `C:\dev\rig-wt\243`, from `main` at 3e0b52b or later.
  - The first commit is this plan, as `docs/superpowers/plans/2026-11-26-generic-tidy.md`. Push, then open a draft PR at once: `Fixes #243`, label `compiler`, milestone `5: The rest of the type system`.
  - Push after each commit. If `origin/main` moves, merge it; never rebase.
  - Commit messages: a summary under 72 characters, an optional body, no trailers. Check that `git config user.email` is the personal one before the first commit.
  - An independent review runs before the PR is marked ready.

## Review Focus

1. **Counts stay each instance's to check.** `check_generic_type` and `names_unbound` skip an array's count today; `walk_names` enters it. Their actions must do nothing when `place.in_count` is set. If they don't, `[size_of(Nope)]$T` is said at the generic, or `h(null, 5)` changes message. Task 1 Step 1 pins both.
2. **`pointee` is only the direct target.** In `*[]void`, the `void` is not a pointer's target. `walk_names` sets `pointee` only when it goes from an `AST_TYPE_POINTER` to its `pointee`, and clears it on every other step. Task 1 Step 1 pins this.
3. **One action per place in the signature walk.** Inside a count, every name (a value's, or a type's that declares nothing) gets `check_count_name`. Outside a count, a type name gets today's `$T` rules. A `$T` never sits in a count (the parser clears `in_param_type` there), but the action still checks `declares` as today.
4. **Order.** Names are met depth first, left to right: an array's count before its element, a cast's type before its value, a literal's type before its values. `Array<Signature_Name>` grows in that order, which decides which name is "used before its $".
5. **The look-ahead stops in time.** `c_style_literal_follows` stops at `;`, at end of file, or at a closer it didn't open. A `,` inside `( )`, `[ ]` or `{ }` in a count (`[f(1, 2)]int`, `[g(.[1, 2])]u8`) is not a C-style literal's. `[..]` is never a literal.

---

### Task 1: One walk over a generic's names

**Files:** Modify `src/check_generic.cpp` (the header comment at :1-9; `collect_type_parameters` :32; `check_generic_type` :103; `check_count_name`/`check_count_type_names`/`check_count_names` :178-262; `check_signature_names` :284; `names_unbound` :534; `names_type`/`names_expression` :1187-1253; `names_statement` :1298). Test in `tests/unit/test_checker.cpp`.

**Interfaces (all file-level `static` in `check_generic.cpp`; these three placed before `collect_type_parameters`, each action below where the walk it replaces was, since `at_signature_name` needs `Signature_Name` and `check_count_name`, and `at_body_name` needs `Name_Walk` and `names_value`, all defined further down):**
```cpp
// Where a name walk_names meets is.
struct Name_Place {
    bool in_count;  // inside an array's count, at any depth: an expression, each instance's to check
    bool pointee;   // a type name that is a pointer's target, *T's T (not *[]T's)
};

// The text of `name`, an Ast_Name or an AST_TYPE_NAME Ast_Type.
static String name_text(Ast *name);

// Walks expression or written type `node` (null: nothing), depth first, left to right: an array's
// count before its element, a cast's, a literal's type before its values. At each Ast_Name (a
// value's, a callee's) and each AST_TYPE_NAME (a $T's too) it calls action(data, name, place).
// What follows a '.', a .NAME and a literal's field names are no names here.
static void walk_names(Ast *node, Name_Place place, void (*action)(void *data, Ast *name, Name_Place place),
                       void *data);
```
`walk_names`' switch lists every `Ast_Kind` and has no `default`:
- `AST_NAME` and `AST_TYPE_NAME` call the action.
- The type kinds and today's expression cases recurse, as `check_count_names` and `check_count_type_names` do now.
- `AST_STRING`, `AST_INT`, `AST_FLOAT`, `AST_BOOL`, `AST_NULL`, `AST_CONTEXT`, `AST_CALLER_LOCATION` and `AST_DOT_NAME` return.
- Every other kind (the statements, the declarations and their parts, `AST_PARAM`, `AST_FIELD` and `AST_CASE`, and the checker's own `AST_BUILTIN` and `AST_ALIAS`) `break`s to a closing `RIG_ASSERT(!"not an expression or a written type")`, as `ast_clone` does. `names_statement` handles every statement before handing an expression on, and a body declares no procedure, struct, union or enum (the parser's `parse_local_declaration` refuses them), so the assert never fires today.
- Entering a count sets `in_count`, which then stays set below it. `pointee` is set only for an `AST_TYPE_POINTER`'s `pointee`.

The actions, one per former walk. Each has the signature `(void *data, Ast *name, Name_Place place)`:

| Former walk(s) | Action | `data` | Notes |
|---|---|---|---|
| `collect_type_parameters` | `at_declared_type` | `Array<Generic_Binding> *` | Adds a type name that `declares` and isn't in a count. |
| `check_generic_type` | `at_generic_type_name` | `struct Generic_Type_Check { Checker *c; Array<Generic_Binding> declared; }` | Today's `AST_TYPE_NAME` case with `place.pointee`. Nothing in a count. |
| `check_signature_names` + `check_count_names` + `check_count_type_names` | `at_signature_name` | `struct Signature_Check { Checker *c; Array<Generic_Binding> declared; Array<Signature_Name> *names; }` | In a count: `check_count_name(c, name_text(name), name->pos, ...)` for an `AST_NAME`, and for an `AST_TYPE_NAME` that doesn't declare. Outside a count: today's `AST_TYPE_NAME` case. |
| `names_unbound` | `at_unbound_name` | `struct Unbound_Search { Array<Generic_Binding> *bindings; Generic_Binding *only; bool found; }` | Sets `found` for a type name, not in a count, that names an unbound binding (or `only`). |
| `names_type` + `names_expression` | `at_body_name` | `Name_Walk *` | `AST_NAME`: `names_value`. `AST_TYPE_NAME`: today's "unknown type" case. It doesn't read `place`: `(void)place;`, as `compiler_module.cpp`'s `(void)results;`, since `-Wextra` warns of an unused parameter. |

The kept entry points and their signatures:
- `collect_type_parameters(Ast_Type *, Array<Generic_Binding> *)`, `check_signature_names(Checker *, Ast_Type *, Array<Generic_Binding>, Array<Signature_Name> *)` and `names_unbound(Ast_Type *, Array<Generic_Binding> *, Generic_Binding *) -> bool` keep their signatures. Each becomes a `walk_names` call with `Name_Place{}`.
- `check_generic_type` loses `pointee`: `static void check_generic_type(Checker *c, Ast_Type *written, Array<Generic_Binding> declared)`. Both callers pass `false` today.
- `names_expression(Name_Walk *w, Ast *node)` keeps its name and takes types too. `names_type` goes, and `AST_VAR`'s `type_expr` uses `names_expression`.
- `match_type` and `add_written_type` stay as they are. They pair a pattern with a `Type`, or write text, so they are not walks with a per-name action, and their `default` asserts (loud, not silent).

- [ ] **Step 1: Pin what the merge could change.** These pin today's behaviour, so run them on the unchanged code: they must PASS. If today's output differs from what is written here, stop and report it. Do not change the expectation.
  - In `generic_refusals`, after the `void` case:
    ```cpp
    // A pointer's target is only its direct one: *[]void's void isn't.
    CHECK_STR(check_source(&a, "e :: (a: $T, b: *[]void, c: **void) { }\nmain :: () { }\n"),
              "1:20: 'void' is only a pointer's target, as in *void\n");
    // An array's count is each instance's to check: one the generic never
    // instantiates says nothing of the type it names.
    CHECK_STR(check_source(&a, "w :: (a: [size_of(Nope)]$T) { }\nmain :: () { }\n"), "");
    ```
  - In `generic_declaration_mistakes`, a case with a count's name and an element's name in one parameter:
    ```cpp
    {"s2 :: (a: [size_of(T)]U, b: $T, c: $U) -> int { return 1; }", "s2(1, 2, 3)",
     "1:20: 'T' is used before '$T' declares it\n1:23: 'U' is used before '$U' declares it\n"},
    ```
  - In `inference_mistakes`, a `CHECK_STR` of its own after the cases' loop (it's a whole source, not a body for the table's `with_generic_max` frame). Inference doesn't look into a count, so the untyped 5 doesn't meet `[size_of(T)]int` as a pattern:
    ```cpp
    CHECK_STR(check_source(&a, "h :: (b: $T, a: [size_of(T)]int) -> int { return 1; }\n"
                               "main :: () { x := h(null, 5); }\n"),
              "2:21: can't work out 'T' for 'h': argument 1 has no type of its own\n");
    ```
- [ ] **Step 2: Write `Name_Place`, `name_text`, `walk_names` and the five actions; rewrite the entry points as above.** Delete `check_count_names`, `check_count_type_names` and `names_type`. Move the comments of the merged functions onto their actions. `check_count_name`, `names_value` and `name_known` stay.
- [ ] **Step 3: Build and run both suites on Windows.** Expected: the build gives no warnings. Temporarily delete one case (say `AST_INDEX`) from `walk_names` and check that the build warns `enumeration value 'AST_INDEX' not handled in switch`, then put it back. All unit tests pass, notably Step 1's, `generic_declaration_mistakes`, `generic_refusals`, `uninstantiated_bodies_resolve_names_only` (every statement and expression kind with a name), `inference_reads_through_types`, `inference_mistakes`, `baked_parameter_mistakes` and `generic_default_places_are_said_once`. `run-tests` all pass.
- [ ] **Step 4: Commit, push, run Linux.** `git commit -m "Walk a generic's names in one walk with an action per name"`

### Task 2: A C-style array literal points to `.[1, 2, 3]`

**Files:** Modify `src/parser.cpp` (`parse_primary`'s grammar comment :339-347 and its `TOKEN_LBRACKET` case :422), `docs/rig-spec.md` (5.17's **Array literals** paragraph :2235, 5.20's third bullet :2315, A.4's paragraph :4759). Test in `tests/unit/test_parser.cpp` (`parses_types_where_values_go` :4578-4584).

**The rule.** These apply only to a `[` where an expression starts. They are C-style literals:
- a `[` whose own brackets hold a `,` (not one nested inside `( )`, `[ ]` or `{ }`): `[1, 2, 3]`, `[1, 2`;
- a `[` whose `]` is not followed by a token that can start a type: `[]` and `[1]` before `;`, `)`, `,`, `==` and so on.

Each is refused at the `[` with:

`an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]`

Everything else is read as a type, as today:
- `[..]`, with any messages it gives today;
- a `[` whose look-ahead reaches `;` or end of file, or a `)` or `}` it didn't open, before its `]`;
- a `]` followed by a name, `*`, `[`, `(` or `$`.

A `[` where a type goes (after `:`, `->`, `#type`, or in a cast) is always a type's.

**Interfaces (file-level `static` in `parser.cpp`, beside `proc_type_follows`):**
```cpp
// Whether a type can start with a token of `kind`: a name, '*', '[', '(' or '$' (parse_type_level).
static bool starts_a_type(Token_Kind kind);
// Whether the '[' the parser is at, where an expression starts, is an array literal written as C
// writes one (the rule above). Scans tokens only, as proc_type_follows does.
static bool c_style_literal_follows(Parser *p);
```
In `parse_primary`'s `TOKEN_LBRACKET` case: if `c_style_literal_follows(p)`, `diag_error(p->diag, t->pos, "an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]")` and return null. Otherwise, call `parse_type_value` as today. Fix the case's comment ("No expression starts with a '['") to say this.

- [ ] **Step 1: Write the failing tests.** In `parses_types_where_values_go`, replace the two pins at :4578-4584 and their comment with these. `body_dump` indents the body 4 columns, so the `[` of `x := [` is at column 10.
  ```cpp
  // An array literal written as C writes one, not .[1, 2, 3] or
  // int.[1, 2, 3] (spec 5.17), is refused at its '[': a ',' in its
  // brackets, or a ']' with no type after it.
  body_dump(&a, "x := [1, 2, 3];", &errors);
  CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
  body_dump(&a, "x := [];", &errors);
  CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
  body_dump(&a, "x := [1];", &errors);
  CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
  body_dump(&a, "x := f([1, 2]);", &errors);
  CHECK_STR(errors, "2:12: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
  body_dump(&a, "x := .[[1, 2], [3]];", &errors);
  CHECK_STR(errors, "2:12: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
  // A ',' nested in the count is no literal's, and a type after the ']'
  // makes it a type; [..] and a '[' where a type goes are always a type's,
  // with their own messages.
  body_dump(&a, "x := [f(1, 2)]int == [g(.[1, 2])]u8;", &errors);
  CHECK_STR(errors, "");
  body_dump(&a, "x := [..];", &errors);
  CHECK_STR(errors, "2:14: expected a type, found ';'\n");
  body_dump(&a, "x := []$T;", &errors);
  CHECK_STR(errors, "2:12: '$' declares a type parameter only in a procedure's parameter, as in "
                    "max :: (a: $T, b: T) -> T\n");
  body_dump(&a, "x := #type [1, 2];", &errors);
  CHECK_STR(errors, "2:18: expected ']', found ','\n");
  body_dump(&a, "x : [1, 2]int;", &errors);
  CHECK_STR(errors, "2:11: expected ']', found ','\n");
  ```
- [ ] **Step 2: Run them and check that they fail.** Build and run `.\bin\unit-tests.exe` (it takes no filter). Expected: the first five fail ("expected ']', found ','" and "expected a type, found ';'"). The rest pass already. If one of them doesn't, its expectation is wrong; fix it against today's output.
- [ ] **Step 3: Implement `starts_a_type` and `c_style_literal_follows`, and use them in `parse_primary`.** The scan starts at the token after the `[` and goes as follows:
  - a `..` there returns false;
  - otherwise it keeps a depth for `(`, `[` and `{`;
  - at depth 0, a `,` returns true;
  - at depth 0, a `]` returns `!starts_a_type(next token's kind)`;
  - at depth 0, a `)` or `}` returns false, and so do `;` and end of file at any depth.

  Update `parse_primary`'s grammar comment to say a `[` that `c_style_literal_follows` is refused.
- [ ] **Step 4: Update the spec.**
  - 5.17, at the end of **Array literals**: "The `.` is part of an array literal: one written as C writes it, `x := [1, 2, 3];` or `x := [];`, is `an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]`, at the `[` (section 5.20 says which `[` begin a type)."
  - 5.20, third bullet: "`[N]T`, `[]T` and `[..]T`: no expression starts with `[`, so one there begins a type. Where it can't be one, there's a `,` in its brackets, or no type follows its `]` (`[1, 2, 3]`, `[]`, `[1]`), it's taken for an array literal written as C writes one: `an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]`, at the `[` (section 5.17);"
  - A.4: in the last sentence, after "since no expression starts with one", add ", unless it's refused as a C-style array literal (section 5.20)".
- [ ] **Step 5: Build and run both suites on Windows.** Expected: Step 1's tests pass. So do `parses_array_literals`, `parses_resizable_array_type`, `parses_array_and_view_types` (its cases at :1617-1622: `g : [3];`, `g : [];` keep "expected a type, found ';'"), and `run-tests`, with no expected file changed. Build the docs site, which makes its pages from the spec and checks their anchors: `cd site && npm ci && npm run docs:build`.
- [ ] **Step 6: Commit, push, run Linux.** `git commit -m "Point a C-style array literal to .[1, 2, 3]"`

**Done when:**
- All tests pass on Windows and Linux, debug and release: unit tests, end-to-end tests, and debugger tests under cdb and gdb.
- No expected file has changed.
- The independent review's findings are fixed.

## Settled by the maintainer

- **Which `[` is a C-style literal:** where a value goes, a `[` with a `,` in its brackets, or a `]` with no type after it. `[1, 2, 3]`, `[]` and `[1]` all get the new message. Where a type goes, nothing changes.
- **The message:** `an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]`.
