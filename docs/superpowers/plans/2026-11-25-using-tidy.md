# Tidy what `using` left: plan

**Goal:** Do what issue #240 asks: the tidying `using` (#225, PR #239) left. Nothing changes what a program means, nor any message, position or exit code.

**Architecture:** The bookkeeping the two walks through `using` fields share (the list of structs searched, each one's way back up, the depth-by-depth queue, the first two places found, the paths read back) moves to one `Field_Walk` in `src/types.h`/`types.cpp`, which `type_find_base`, `type_find_member` and the checker's `find_through_using` all use; each keeps its own search of a struct. Both builders of an `Ast_Member` chain use one helper. `check_lookup` hands the walk's result to its caller, so `check_using_name` and a callee or member operand reuse it.

**Tech Stack:** C-style C++23, no STL, pinned clang 23.1.5 (`build.bat`, `build.sh`).

**Spec:** issue #240 (`gh issue view 240`); `docs/rig-spec.md` 3.21 and `src/checker.h`'s using section, which stay as they are (comments that name a removed function are updated); `docs/superpowers/plans/2026-11-25-using.md` for how the walks came to be.

## Global Constraints

- No behaviour change: no expected file (`tests/run`, `tests/errors`, `tests/debug`) changes. One that does is a bug to find, not a file to regenerate.
- Code is study material: each moved or new piece keeps a comment in the style around it saying what it does and why. Comments naming a removed type or function (`Base_Search`, `Using_Search`, `using_search_needed`, `through_using`, ...) are fixed in the same commit.
- Unity builds: no new source file; file-level `static` names stay unique across `src/all.cpp`.
- `-Wall -Wextra`, no warnings, debug and release, Windows and Linux.
- Every commit passes, on both hosts: `bin\unit-tests.exe`, `bin\run-tests.exe` (which runs the debugger tests last, cdb on Windows, gdb on Linux: they must PASS, not SKIP).
  - Windows (PowerShell, `C:\dev\rig-wt\240`): `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c build.bat`, then `.\bin\unit-tests.exe`, `.\bin\run-tests.exe`.
  - Linux (WSL, `~/rig-wt/240`, never through `/mnt/c`), from Git Bash after the push:
    ```bash
    MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
    cd ~/rig-wt/240 && git pull -q --ff-only
    RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
    EOF
    ```
    (`git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/240 issue/240-using-tidy` the first time.)
  - Before the final review: `build.bat release` and `./build.sh release`, each followed by both test programs.
- Work flow: branch `issue/240-using-tidy`, worktree `C:\dev\rig-wt\240`, from `main` at 2326d81 or later. This plan is the first commit, `docs/superpowers/plans/2026-11-25-using-tidy.md`; push and open a draft PR at once: `Fixes #240`, label `compiler`, milestone `5: The rest of the type system`. Push after each commit; merge `origin/main` if it moves (never rebase). Commit messages: a summary under 72 characters, optional body, no trailers. Check `git config user.email` is the personal one before the first commit. An independent review runs before the PR is marked ready.

## Review Focus

1. **Path order in two-way messages.** `'x' is ambiguous: 'a.x' or 'b.x'` and `'*Twice' converts to '*Entity' two ways, through 'a' and through 'b'` name the first two places in walk order (field order, an anonymous member's fields in its place). A shared `field_walk_found` must keep the first two, not the last two.
2. **Ownership of a lookup's paths.** `check_lookup` now hands `Using_Name`'s arrays to its caller. Every path out (found 0, 1, 2, -1; a callee that turns out to be a procedure; `check_member`'s enum and tagged-union returns) must free them exactly once. No test sees a leak or a double free: read each path.
3. **A late field made before it's read.** The chain helper reads `s->fields[index].type`; `check_using_name` must still call `check_late_field` on the last field first.
4. **-1 still hides the top level.** A walk that met a circle returns -1; `check_lookup` must still return a parameter (not fall through to a procedure or global of the name), so only the circle is said. Pinned for a bare name and an assignment today; Task 3 pins it for a callee and a member operand.
5. **Nothing walks before its depth.** Moving `reach` out of the step must not make `find_through_using` lay out or take a struct earlier than today (`using_walks_lay_out_only_the_structs_they_enter`, `using_reaches_a_struct_under_way` pin this).

---

### Task 1: One walk's bookkeeping for `type_find_base`, `type_find_member` and `find_through_using`

**Files:** Modify `src/types.h` (near `type_find_member`, :303), `src/types.cpp:128-283`, `src/check_expr.cpp:3103-3352`.

**Interfaces (produces, in `types.h`):**
```cpp
struct Field_Step {   // one struct a walk searches
    Type *type;       // the struct; the name walk's is null until it takes the step at its depth, and for one it doesn't search
    s64 outer;        // the step it was reached from (holding it or pointing at it), an index into list; -1 for a root
    s32 field;        // its field there
    s32 depth;        // the depth it's searched at; an anonymous member at its outer's
    s32 root;         // which root it was reached from
};
struct Field_Walk {
    Array<Field_Step> list;  // every step, searched or to search: each path's way back up
    Array<s64> level;        // the steps searched at this depth
    Array<s64> next;         // those to search at the next
    s32 found;               // places found so far; the first two kept in found_in/found_field
    s64 found_in[2];
    s32 found_field[2];
};
void field_walk_add_root(Field_Walk *w, Type *t, s32 root);  // a step at depth 0, into list and level
bool field_walk_needed(Field_Walk *w, Type *type, s32 depth); // today's base_search_needed / using_search_needed
void field_walk_found(Field_Walk *w, s64 at, s32 field);      // counts a place, keeps the first two
void field_walk_next_depth(Field_Walk *w);                    // level becomes next; next emptied (buffers swapped)
s32 field_walk_path(Field_Walk *w, s32 k, Array<s32> *path);  // k-th place's field indices from its root down; returns the root
s32 field_walk_paths(Field_Walk *w, Array<s32> *path, s32 *path_root, Array<s32> *other, s32 *other_root);
                      // min(found, 2), path filled for >= 1, other for 2
void field_walk_free(Field_Walk *w);
```

- [ ] **Step 1: Move the shared parts into `types.cpp`.** `Member_Search`, `Base_Search`, `Base_Walk`, `base_search_needed`, `base_found_path` go; `type_find_base` and `search_base_struct(Field_Walk *w, Type *base, s64 at)` use `Field_Walk` (root 0). `type_find_member` uses `w.list` as its queue and `field_walk_path` for its one path (no depth, no level). Signatures of `type_find_member`, `type_find_base` don't change. `field_walk_needed`'s comment merges the two old ones: why it keeps the walk small (two paths at a depth suffice to say "two"), and that in the name walk it ends cycles through pointers.
- [ ] **Step 2: The checker's walk uses it.** In `check_expr.cpp`, `Using_Search`, `Using_Walk`, `using_search_needed`, `using_found_path` go. `Using_Reach` stays, but out of the step: `static Using_Reach take_using_struct(Checker *c, Field_Walk *w, s64 at, String name)` returns it, and `find_through_using` keeps an `Array<Using_Reach> reach` index-aligned with `w.level`, refilled each depth. `static void search_using_struct(Field_Walk *w, s64 at, String name, bool queue)`. `find_through_using`'s signature, its three passes per depth and their order stay exactly as they are.
- [ ] **Step 3: Build and run both suites on Windows.** Expected: build with no warnings; `unit-tests` all pass (notably `using_walks_end_on_cycles`, `base_walks_end_on_doubling_chains`, `using_names_at_one_depth_are_ambiguous`, `pointers_convert_through_using_fields`, `using_walks_lay_out_only_the_structs_they_enter`, `using_reaches_a_struct_under_way`, and the anonymous-member tests for `type_find_member`); `run-tests` all pass.
- [ ] **Step 4: Commit and push; run Linux.** `git commit -m "Share the using walks' bookkeeping in one Field_Walk"`

### Task 2: One builder for a member chain

**Files:** Modify `src/check_expr.cpp` (`check_using_name` :3415, `check_member`'s chain :3602-3620).

**Interfaces (produces, file-level `static` in `check_expr.cpp`, before `check_using_name`):**
```cpp
// The member reaching field `index` of struct s from operand: named as the field (none for an
// anonymous member), its index and type the field's, at pos and start. The caller makes a late
// field's type first (check_late_field).
static Ast_Member *member_link(Checker *c, Ast *operand, Type *s, s32 index, Position pos, Position start);
// operand followed through every field of path but the last, from struct *s: the chain (operand
// itself for a one-field path), *s left at the struct the last field is in, stepping through a
// pointer field to its pointee.
static Ast *member_chain(Checker *c, Ast *operand, Type **s, Array<s32> path, Position pos, Position start);
```

- [ ] **Step 1: Use them.** `check_member`: `m->operand = member_chain(c, m->operand, &s, path, m->pos, m->start);` then `m->index` and the late field as today. `check_using_name`: `member_chain` from the parameter's `Ast_Name` with `name->pos` for both positions (as today: `ast_new` sets `start` to `pos`), then `check_late_field` on the last field, then `member_link` for it. The comment on the chain moves to `member_chain`; the two call sites say only what's theirs.
- [ ] **Step 2: Build and run both suites on Windows.** Expected: all pass, notably `using_chains_follow_pointer_fields`, `using_parameters_name_their_fields`, and run tests `using_null_second_hop`, `using_parameter_null`, `anonymous_member_null` (their null-pointer positions are the chain's `pos`).
- [ ] **Step 3: Commit and push; run Linux.** `git commit -m "Build a member chain through one helper"`

### Task 3: Walk a `using` parameter's name once; `using_params`' comment

**Files:** Modify `src/check.h` (Checker :30-32, `check_lookup` :91-101), `src/checker.cpp:376-408`, `src/check_expr.cpp` (`check_name` :965-992, `check_callee` :1246, `check_addressable` :2805, `check_using_name` :3396-3460, `check_member` :3518); test `tests/unit/test_checker.cpp`.

**Interfaces (produces, in `check.h`):**
```cpp
// Where check_lookup found a name among the fields the using parameters reach (spec 3.21):
// check_find_using's result, kept so the walk runs once per use.
struct Using_Name {
    s32 found;         // 0: not there; 1; 2: ambiguous; -1: a circle or a layout found wrong, said
    s32 root;          // the first path's parameter, an index into Checker.using_params
    s32 other_root;    // the second's
    Array<s32> path;   // the first path's field indices from its parameter's struct down
    Array<s32> other;  // the second's
};
Ast *check_lookup(Checker *c, String name, Using_Name *through);  // through may be null
```
In `check_expr.cpp`:
```cpp
static Type *check_using_name(Checker *c, Ast_Name *name, Using_Name *through);  // no walk; frees through's arrays
static Type *check_named(Checker *c, Ast_Name *name, Ast *decl, Using_Name *through); // check_name after its lookup
```

- [ ] **Step 1: Pin today's messages for the paths that change.** In `using_parameter_names_come_between_locals_and_globals`, a new `CHECK_STR`:
  ```cpp
  CHECK_STR(check_source(&a, "A :: struct { x: int; }\n"
                             "A2 :: struct { x: int; }\n"
                             "f :: (using a: *A, using b: *A2) { x(); }\n"
                             "g :: (using a: *A, using b: *A2) -> int { return x.y; }\n"
                             "k :: (using a: *A, using b: *A2) { p := *x; }\n"
                             "main :: () { }\n"),
            "3:36: 'x' is ambiguous: 'a.x' or 'b.x'\n"
            "4:50: 'x' is ambiguous: 'a.x' or 'b.x'\n"
            "5:42: 'x' is ambiguous: 'a.x' or 'b.x'\n");
  ```
  In `using_walks_lay_out_only_the_structs_they_enter`, `reaching` gains `{"(using b: *B)", "h(); return 2;"}` and `{"(using b: *B)", "return g.k;"}` (loop bound 4 to 6), each expecting only `1:1: 'A' is defined in terms of itself\n`; extend the comment above to say a callee and a member's operand too. Run on the unchanged code: these pin today's behaviour, so they PASS. If today's output differs, stop and report it: it's a question for the maintainer, not something to change here.
- [ ] **Step 2: `check_lookup` fills `Using_Name`.** It sets `through->found = 0`, runs `check_find_using` into `through` when given (into locals freed at once when null), and returns `c->using_params[through->root]` whenever found != 0, as today. When found is 0 nothing is left allocated. Rewrite its comments in `check.h` and `checker.cpp` for the new parameter.
- [ ] **Step 3: Callers pass it on.**
  - `check_name` = `check_lookup` + `check_named`; `check_named` calls `check_using_name(c, name, through)` when `through->found != 0`.
  - `check_addressable`: `Using_Name` in place of the bool, passed to `check_using_name`.
  - `check_callee`: looks up with `&through`; `t = check_expression(c, name)` becomes `t = name->type = check_named(c, name, decl, &through)`.
  - `check_member`: a bare name operand is looked up with `&through`; when it's neither an enum nor a tagged union, `name->type = check_named(...)`, then what `check_operand` does after: a type that `type_waits_for_one` goes to `check_default_type` and the operand's type is null. Other operands keep `check_operand`.
  - `check_using_name` reads `through` instead of walking, and frees its arrays on every path; its comment says the walk was `check_lookup`'s.
  - The callers passing null (`tagged_member_named`, `names_a_member`, `names_a_void_member`, `report_member_on_pointer`) stay: they only ask what a name declares, and a miss lays nothing out.
- [ ] **Step 4: Build and run both suites on Windows.** Expected: all pass, including Step 1's, `using_parameter_names_come_between_locals_and_globals`, `value_using_parameters_are_read_only` (default-value and read-only refusals), `using_parameters_name_their_fields`, and run tests `using_parameters`, `using_run` (a #run's refusal reads `name->through`).
- [ ] **Step 5: Commit and push; run Linux.** `git commit -m "Walk a using parameter's name once per use"`
- [ ] **Step 6: Move `Checker.using_params`' comment above the field**, in the style of `Ast_File.operating_system` (`ast.h:826-832`) and `Load_Options.stdin_for` (`load.h`): a `//` paragraph at the field's indent, then `Array<Ast_Param *> using_params;` with no trailing comment. Text unchanged. Build, `unit-tests`, commit: `git commit -m "Put using_params' comment above the field"`; push; run Linux.

**Done when:** all tests pass on Windows and Linux, debug and release (unit tests, end-to-end tests, debugger tests under cdb and gdb); no expected file changed; the independent review's findings are fixed.

## Settled by the maintainer

Nothing needed deciding. `using_params`' comment moves above the field rather than the field being renamed, as other structs in the repo already do.
