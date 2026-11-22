# Milestone 5, the rest of the type system: design

## Intent

Milestone 5 finishes Rig's type system, so that a real library can be written cleanly in Rig. The measuring stick is the kind of library the maintainer named: an entity-component system. That means typed containers, types passed as arguments, sets of flags, events handled by a switch that has to cover every case, and vector maths written with operators. **No ECS is built.** The proof shipped in the repo is Basic: its untyped tables, its `sort` and its per-type maths become generic, and Preload's and Basic's "constants until enums" become enums. Milestone 5 is done when that code reads cleanly. A test program in the ECS style also checks the rest: flags, tagged unions and operators.

## Scope

**In:**
- enums and `enum_flags`;
- the switch (`if x == { case ... }`) and `#complete`;
- C unions;
- tagged unions;
- `using`;
- generics (`$T`, generic structs, type aliases, `#modify`);
- operator overloading;
- Basic and Preload rewritten on top of these.

`using` is the one addition to the roadmap's list. The maintainer agreed to it: it is small, it is about types, it is already a reserved word, and it gives `Type_Info` the `t.size` it lacks today.

**Out (non-goals):**
- **`#insert`** (code built as a string while compiling). It's metaprogramming, not types. It gets an issue of its own in a later milestone, reconsidered once generics and `#modify` show what they can't do.
- **Constraints, traits or interfaces on generics.** The original design says no declared constraints; `#modify` is the check.
- **Overloading procedures by name.** Only operators are overloaded.
- **Variadic type parameters** (`$types: ..Type`).
- **`Type` values at run time.** A `*Type_Info` already serves.
- **Methods, closures and inheritance.**
- **`operator []`.**
- **`using` as a statement in a body.**
- **Generic `#foreign`, `#c_call` or `#compiler` procedures.**

**Anti-goals** (failures even if the features technically work):
- An error inside generic code that doesn't name the instance and the call that made it.
- Layouts that C can't read: enums, unions and tagged unions are documented for C, as structs are.
- Anything that works compiled but not in the interpreter (`#run`), or that the debugger can't show.
- Making today's simple code harder: everything existing keeps compiling, except Preload's and Basic's renamed constants and procedures, which are listed in their issues.

## Constraints

- Jai's syntax and meaning unless there's a reason to differ, and the reason is written down.
- Every feature works compiled, interpreted and in a `#run`, on Windows and Linux. Debug information covers it: the PDB and DWARF, and the VS Code debugger tests under cdb and gdb.
- `docs/rig-spec.md` is the source of truth. Each issue updates:
  - the spec, the grammar (Appendix) and the keyword and directive lists;
  - `site/features.md`;
  - the VS Code grammar for new keywords and directives;
  - one README line per feature at most.
- Error messages in the spec's style: one plain sentence, names quoted, with an "as in ..." example where it helps.
- One PR per issue, done by the usual workflow, in the order below.
- LLVM stays at the pinned 23.1.5.

## Approaches considered (for the order)

- **(a) Small to big, in dependency order. Chosen.** Each step ships something useful, and the later ones build on the earlier ones: the tagged union's switch is the switch, generic structs come after generic procedures, and operators come after generics so `Vec(T)` can have them.
- **(b) Generics first.** The biggest payoff for Basic comes soonest, but the riskiest work happens with nothing landed yet, and enums would arrive after the generic code that wants them.
- **(c) Everything designed up front and built in parallel.** The switch, unions, generics and operators all touch the same checker paths (operators, calls, member access), so the pieces would keep colliding.

## The design, feature by feature

Everything below is a design choice of this milestone unless marked *(original design)*: those come from `docs/superpowers/specs/2026-11-11-rig-language-design.md` (sections 6.8, 6.10 and 6.11). The maintainer settled the order and scope.

### 1. Enums and `enum_flags`

- **Declaration:**

  ```
  Mode :: enum { ALLOCATE; RESIZE; FREE; FREE_ALL; }
  Small :: enum u8 { A; B :: 5; C; }
  ```

  - The integer type is chosen after `enum` *(original design)*; without one it is `int`. A C binding writes `enum s32`.
  - The values start at 0 and go up by one.
  - `NAME :: constant expression` sets a value, and the next member continues from it. Inside the enum's body, earlier members are plain constants of its integer type, as in Jai, so `LAST :: C + 3` and, in `enum_flags`, `ALL :: A | B | C` work.
  - Each value must fit the chosen type. Two members may share a value.
- **`enum_flags`** is written the same way, but the values go 1, 2, 4 and so on, and an explicit value is allowed.
  - `| & ^ ~` work between values of the same flags type.
  - The untyped constant `0` converts to a flags type, as the empty set, so `(f & .VISIBLE) != 0` works. Rig's conditions must be `bool`, which is why `0` gets this exception.
- **Naming a member:**
  - `Mode.FREE`.
  - `.FREE` alone where the type is known, as Jai allows: a typed declaration, an assignment, an argument, a return, a field of a literal, a `case`, the other side of `==` or `!=` from an enum value, and the other operand of `| & ^` from a flags value (`f = .A | .B` takes its type from `f`, and `.A | .B` with no type around it is the error below).
  - `.FREE` with nothing to give it a type is `this member's enum isn't known; write it with its enum, as in Mode.FREE`.
- **Conversions:**
  - An enum is a type of its own; nothing converts to or from it implicitly, except `0` to a flags type.
  - `cast(int) m` and `cast(Mode) 3` cast between an enum and any integer type, unchecked: a value with no member is allowed, as C allows.
  - The comparisons `== != < <= > >=` work between values of the same enum. Arithmetic doesn't.
- **Members are constants.** They work in `#if`, in array counts and in `#run` results.
- **Inside an enum's own scope**, its member names don't hide outer names. `Mode.FREE` and `.FREE` are the only ways to reach a member.
- **Type information:**
  - A new tag, `.ENUM`, numbered 11 as Jai's is. The tags themselves become `Type_Info_Tag` in this issue (below), so the `TYPE_INFO_*` constants become `Type_Info_Tag.INTEGER` and so on.
  - A new struct: `Type_Info_Enum :: struct { using info: Type_Info; name: string; underlying: *Type_Info; is_flags: bool; names: []string; values: []s64; }`. The `using` arrives in step 5; until then it's `info: Type_Info;`.
- **Basic's `print`:**
  - A member prints as its name.
  - A flags value prints as its names joined by ` | `, with any leftover bits as a number, and `0` when empty.
  - A value with no member prints as its number.
- **C and debug information:**
  - An enum crosses into C as its integer type.
  - The PDB and DWARF describe it as an enumeration, so the debugger shows names.
- **Conversions in this issue:**
  - Preload's allocator modes become `Allocator_Mode :: enum { ALLOCATE; RESIZE; FREE; FREE_ALL; }`, and the allocator procedure's `mode` parameter takes it.
  - The `Type_Info` tags become `Type_Info_Tag :: enum { INTEGER; ... }`, and `Type_Info_Array`'s `array_kind` becomes `Array_Kind`.
  - Basic's table entry states become an enum.
  - `OS` and its values become an enum, `Operating_System`, if the compiler's `OS` constant allows it; otherwise that stays as it is, with the reason given.
  - Programs that use the old names are updated, Basic and the tests included.
  - This is a breaking change before 1.0, so it gets its own line in the release notes.

### 2. The switch

- **Syntax** (Jai's):

  ```
  if x == {
      case 1;     a();
      case 2;     b(); #through;
      case 3;     c();
      case;       d();
  }
  ```

  - `if EXPR == {` starts a switch; an `if` whose condition is followed by `{` stays an ordinary `if`. The parser tells them apart by the `{` right after `==`.
  - `EXPR` is evaluated once.
  - Each `case VALUE;` runs the statements up to the next `case` or the closing `}`, in a block of their own.
  - There is no fallthrough unless the case ends with `#through;`. That's a new directive, and it must be the case's last statement.
  - `case;` is the default case. The original design calls this "an explicit `else`": it's spelled `case;` here as Jai spells it.
- **What it switches on:** integers, enums, `bool`, `string` (compared as `==` compares strings), and pointers. Not floats, structs, views or arrays.
- **The cases:**
  - Case values are constants that convert to the switched expression's type.
  - Two cases with the same value are an error at the second.
  - At most one `case;`, and it can come anywhere.
- **`#complete`:** `if #complete x == { ... }` *(original design)*, for enums only.
  - Every value the enum's members have must have a case, `case;` or not: `#complete` means "tell me when a member has no case". Members sharing a value are covered by one case of either name.
  - A missing member is `this switch is #complete, but 'Mode.FREE_ALL' has no case`, listing all the missing members in one message.
  - `#complete` on a non-enum is an error.
- **No case at run time.** A `#complete` switch without `case;` can still meet a value with no member (`cast(Mode) 7`, or memory C wrote), and so can a tagged union's switch with a tag C wrote. That's a runtime error, `runtime error: no case for value 7 of 'Mode'`, at the `if`, checked with bounds checks on or off, compiled and interpreted, as the null check is (spec 8.4). A switch without `#complete` and without `case;` just does nothing.
- **`break` and `continue`** act on the enclosing loop, as they do inside an `if`.
- **"Reaching the end"** (spec 6.11): a switch doesn't reach its end when it has a `case;` (or is `#complete` and covers everything) and none of its cases reaches its end.

### 3. C unions

- **Declaration:** `Value :: union { i: s64; f: float64; p: *void; }`.
  - Every member is at offset 0.
  - The size is the largest member's, rounded up to the largest alignment: C's layout.
  - Any member can be read or written at any time. Reading a member other than the last one written gives its bytes reinterpreted, the program's business as in C.
- **Anonymous members** inside a struct or union, needed for C bindings:

  ```
  Event :: struct { kind: s32; union { key: Key_Event; mouse: Mouse_Event; }; }
  ```

  - The members are reached directly, `e.key`.
  - The same works for an anonymous `struct { ... }` inside a union.
- **Literals:** `Value.{f = 1.5}` gives exactly one member, by name; the rest of the bytes are zero.
- **Comparison:** like structs, unions are not compared.
- **Type information:** `Type_Info_Struct` gains `is_union: bool`, appended at the end, and a union's members all have offset 0. Anonymous members show up as members with an empty name. `print` writes a union as `union` and its size, not its members.
- **Debug information:** a union type.
- **C:** a union passes to C by value and by pointer, as a struct does, with C's ABI for unions.

### 4. Tagged unions

- **Declaration:**

  ```
  Shape :: union #tagged { circle: Circle; rect: Rect; empty: void; }
  ```

  - `union #tagged` with an optional tag type, as in `union #tagged u32`.
  - A member of type `void` carries no data.
- **Layout** *(original design)*:
  - First the tag: the smallest unsigned integer type that counts every member, or the chosen one.
  - Then the members, overlapping at the next offset aligned to the largest member's alignment.
  - The size is rounded up to the overall alignment. That's a C struct holding an integer and a union, documented so that C can read it.
  - The tag values are 0, 1, 2 and so on, in member order.
- **Zero value:** a zeroed tagged union holds its first member, zeroed. Variables declared without a value are zeroed.
- **Making one:**
  - `Shape.{circle = Circle.{2}}`, or `.{circle = c}` where the type is known.
  - The literal gives exactly one member, by name.
  - A `void` member is made with `Shape.empty`, or `.empty` where the type is known. (`Shape.{empty}` already means a literal whose first field is the variable `empty`.)
- **Reading one** *(original design: only inside a switch case that binds it)*:

  ```
  if s == {
      case .circle;  area = it.r * it.r * PI;
      case .rect;    area = it.w * it.h;
      case .empty;   area = 0;
  }
  ```

  - Inside a case, `it` is the member, as `it` is a loop's element. It's a place when the switched expression is one, so `it.r = 3;` writes into the union.
  - An inner `it` hides an outer one, as nested loops' do.
  - `s.circle` anywhere is `a tagged union's member is read in a switch: if s == { case .circle; ... it ... }`.
- **Every case covered** *(original design)*: a switch on a tagged union must name every member or have `case;`. A missing member is `this switch doesn't handle 'Shape.empty'; add its case or a 'case;'`. `#complete` adds nothing here.
- **Testing the tag without a switch:** `s == .empty` and `s != .circle` are `bool` expressions, for a quick test.
- **Comparison and C:**
  - Tagged unions aren't compared otherwise.
  - They pass to C as the struct they're laid out as.
- **Type information:** a new tag `.TAGGED_UNION`, numbered 32 (Jai has no such kind, and 32 is past every number Jai uses), and `Type_Info_Tagged_Union :: struct { using info: Type_Info; name: string; tag_type: *Type_Info; tag_offset: int; members: []Type_Info_Struct_Member; }`.
- **Basic's `print`:** writes `.circle({r = 2})` (with the member's printing inside), or `.empty`.
- **Debug information:** a struct of the tag and a union of the members.

### 5. `using`

- **On a field:** `Player :: struct { using entity: Entity; health: int; }`.
  - `p.position` means `p.entity.position`, at any depth.
  - `p.entity` still works.
  - The field may also be a pointer, `using parent: *Node;`.
- **On a parameter:** `move :: (using e: *Entity, dx: float)`, so the body's `position` means `e.position`.
  - The parameter's read-only rule still applies to a value parameter's fields.
- **Name lookup:**
  - A struct's own fields come first, then each `using` field's fields, depth by depth.
  - Two names at the same depth are an error where the name is used: `'position' is ambiguous: 'a.position' or 'b.position'`.
  - A parameter's `using` names come after locals and the other parameters, and before globals.
- **Conversion:** a `*Player` converts implicitly to a `*Entity` when `Player` has `using entity: Entity`, at any offset, through nested `using` fields too, but not through pointer fields. Jai does this, and it's what makes composition stand in for inheritance.
- **Literals:** fields reached through `using` can't be named in a literal. You write `Player.{entity = .{...}, health = 3}`.
- **Type information:** unchanged; the member is described as an ordinary field.
- **Preload:** `Type_Info`'s kind structs switch to `using info: Type_Info;`, so `t.size` works; `t.info.size` still works.

### 6. Generic procedures

- **`$T` in a parameter's type** declares `T`, and the argument's type gives it a value: `max :: (a: $T, b: T) -> T`.
  - The first `$T` declares it, and every other `T` must agree.
  - An untyped constant argument takes its default type only when no other argument fixes `T`.
- **Explicit type parameters:** `get :: (w: *World, $T: Type) -> *T`, called as `get(w, Position)`. The argument is a type.
- **Constant parameters:** `$N: int` or any constant type. The argument must be a constant, and `N` is a constant in the body (so `[N]T` works).
- **One copy per distinct set of values** *(original design)*. Each instance is checked on its own. Copies are shared program-wide, so the same `max(int)` is made once.
- **Errors** in an instance say where the instance was made. The message comes first, then `in 'max' with T = Vec2, called at main.rig:12:5`, following the call chain the interpreter's errors use.
- **Uninstantiated bodies** are parsed and their names resolved where that doesn't depend on `T`, but they aren't type-checked, as in Jai. `rig check` on a library that never calls its generic code misses type errors in it. This is accepted, and the tests use every generic they ship.
- **Types where a value goes.** Generics put types where values go: `get(w, Position)`, `T == int`, `Table(string, int)`, `T = s64;` in `#modify`, and aliases. As in Jai:
  - A name is resolved after parsing: one naming a type is a type.
  - `[N]T`, `[]T` and `[..]T` start a type wherever an expression goes, since `[` starts no expression today.
  - `*X` is a pointer type when `X` is a type, and an address otherwise.
  - A procedure type in a value position is written `#type (int) -> int`, Jai's directive, since `(int)` there reads as a parenthesised expression.
  - The spec's grammar (Appendix) and section 5 change to match.
- **Testing what kind of type `T` is.** A `#run` inside an instance sees that instance's `T`, so `#if #run type_info(T).tag == .ENUM { ... }` picks code by kind. That's existing machinery, so no new built-in tests are added. The same works in `#modify`, which runs in the interpreter anyway.
- **`T` as a value in the body:**
  - `T == int` and `T != string` are constant `bool`s, so `#if T == string { ... }` picks code per instance.
  - `size_of(T)` and `type_info(T)` work.
- **Not a value:** a generic procedure can't be taken as a value (`'max' is generic; call it, or wrap one instance in a procedure`), can't be `#foreign`, `#c_call` or `#compiler`, and can't be variadic over types.
- **The rest:**
  - A generic procedure's `..T` parameter works: every argument converts to the one `T`, fixed by the first argument.
  - Default parameters work as usual.
  - `#caller_location` works too.
- **Debug information and type information:** an instance is named `max(T = int)`, both in symbol names (mangled to be valid) and in the debugger.

### 7. Generic structs and type aliases

- **Declaration:** `Table :: struct (K: Type, V: Type) { ... }` and `Ring :: struct (T: Type, N: int) { items: [N]T; ... }`, in Jai's syntax: no `$`, since the parameters are always given.
- **Using one:**
  - `Table(string, int)` names an instance. The arguments are types or constants.
  - Instances with the same arguments are the same type.
- **In parameters:** `table_set :: (t: *Table($K, $V), key: K, value: V)` takes any instance and binds `K` and `V`.
- **Literals:** `Table(string, int).{...}` is allowed. The literal grammar (5.17) takes an instance, `Name(args)`, before `.{`, as well as a plain name.
- **Inside the struct:** `K` and `V` are constants, and a field can hold a pointer to the struct's own instance, `next: *List(T)`.
- **Type aliases:** `Name :: TypeExpression;`.
  - Examples: `Int_Table :: Table(string, int);`, `Allocator_Proc :: (mode: Allocator_Mode, ...) -> *void;`.
  - An alias is the same type, not a new one.
  - Today a constant can't hold a type, so this is new syntax at the top level and in bodies. A name used where a type is expected looks through aliases.
- **Type information:** `Type_Info_Struct.name` is `Table(string, int)`.
  - `Type_Info_Struct` gains `parameters: []Any`, appended, holding the instance's arguments: types as `*Type_Info`, constants as their values.
- **Basic's `print`** prints the instance's name.
- **Debug information** uses the same names.

### 8. `#modify`

- **Syntax** (Jai's):

  ```
  sum :: (values: []$T) -> T #modify { return T != string, "sum needs numbers"; } { ... }
  ```

  - `#modify` sits after the signature and before the body. The same goes for a generic struct, after its parameters: `struct (T: Type) #modify { ... } { ... }`.
- **When and how it runs:**
  - Once per new instance, by the interpreter, before the instance is checked.
  - Inside the block, `T` and the other parameters are variables of type `Type`, or of their constant type, and the block may assign to them (`T = s64;`) to change the instance it makes.
  - It returns `bool`, or `bool, string`.
- **When it returns `false`:** the instance is refused. The error is at the call, saying `'sum' refused T = string: sum needs numbers`, or without the message.
- **The `Type` type** exists only for `$` parameters, `#modify` and type aliases. A variable of type `Type` outside `#modify` is `'Type' only names types while compiling; a *Type_Info describes one at run time`.
  - Inside `#modify`, `type_info(T)` lets the block inspect the type: its fields, its tag and so on.

### 9. Operator overloading

- **Declaration:** `operator + :: (a: Vec2, b: Vec2) -> Vec2 { ... }`, a top-level declaration.
- **Which operators can be overloaded:**
  - the binary `+ - * / % & | ^ << >>` and `== != < <= > >=`;
  - the unary `-` and `~`.
  - Not `&& || !`, assignment, `.`, `[]`, `cast` or the pointer operators.
- **What they can apply to:**
  - At least one operand's type must be a struct, a union, a tagged union or an enum (not an `enum_flags` type for `| & ^ ~`, which are built in). Never only built-in types *(original design)*.
  - An overloaded operator on an enum is the one way to give an enum arithmetic.
- **`#symmetric`** *(original design)*: `operator * :: (v: Vec2, s: float) -> Vec2 #symmetric` also serves `s * v`.
- **Choosing one:**
  - Of all the `operator +` declarations, the one whose parameter types match the operands' types exactly, untyped constants converting as for arguments.
  - Generic operators work: `operator + :: (a: Vec($N), b: Vec(N)) -> Vec(N)`.
  - Two that match equally is an error at the use, naming both. No match is today's error for that operator.
- **Derived forms:** `a += b` is `a = a + b` when `+=` isn't declared, and `!=` is `!(a == b)` when `!=` isn't. Declaring `+=` directly is allowed (it takes `*Vec2` first).
- **Precedence and order of evaluation** stay the built-in operator's. The operands are evaluated left to right, then the procedure is called.
- **Not a value:** an operator procedure can't be taken as a value or called by name.

### 10. Basic and Preload, generic

- **Tables:** `Table(K, V)` replaces `Table` and `Int_Table`.
  - Its procedures are `table_init`, `table_set(*t, key, value)`, `table_find(t, key) -> *V`, `table_remove`, `table_entry` and `table_free`.
  - It's hashed by `hash(key)`, a generic chosen with `#if` for strings, integers, enums and pointers, and fails to compile through `#modify` for anything else.
  - The old untyped procedures are removed.
- **Sorting:** `sort(items: []$T, less: (a: T, b: T) -> bool)` and `binary_search` become typed. `less_int`, `less_string` and the other `less_*` procedures stay, typed, as ready-made comparisons, since a generic procedure can't be passed as a value.
- **Maths:** all of `math.rig` goes generic. `min`, `max`, `clamp` and `abs` replace `min_int`, `max_float32` and the rest. The float functions in pairs (`sqrt_float32` and `sqrt_float64`, `sin_`, `pow_`, `floor_` and the others) become one each, `sqrt(x: $T)`, restricted to floats by `#modify`. The old names are removed and listed in the release notes.
- **The nine resizable-array procedures** (spec 3.17) stay built in. As ordinary Preload procedures they would lose two things the spec promises: a caller's `#no_abc` turning off their bounds checks (7.7), and runtime errors reported at the caller's line (3.17). Only the "Why they are built in" paragraph changes: generics could express their signatures now, but not those two behaviours.
- **Basic's README and the spec** list the new signatures.
- **The ECS-style test program** in `tests/run`:
  - typed component storage, a generic `Table` from entity to component, and `get(world, e, Position)`;
  - an `enum_flags` signature;
  - a tagged-union event queue handled by a switch that covers every case;
  - `Vec2` operators.

  It checks the milestone's intent end to end.

## Issues, in order

1. Enums and `enum_flags` (section 1), with Preload and Basic switched to them.
2. The switch, `#complete` and `#through` (section 2).
3. C unions and anonymous members (section 3).
4. Tagged unions (section 4).
5. `using` (section 5), with Preload's `Type_Info` structs switched to it.
6. Generic procedures (section 6), including the `Type` type for `$T: Type`.
7. Generic structs and type aliases (section 7).
8. `#modify` (section 8).
9. Operator overloading (section 9).
10. Basic and Preload, generic, plus the ECS-style test program (section 10).
11. *(A later milestone, not 5.)* `#insert`.

The type information tags follow Jai's numbering where Jai has one. Section 3's `is_union` and section 7's `parameters` are appended to `Type_Info_Struct`, so earlier fields keep their offsets.

## Settled after a builder's read

- Earlier enum members are plain integer constants inside the enum's body (section 1).
- `.NAME` takes its type from the other operand of a flags `| & ^` (section 1).
- A `#complete` switch counts values, not names; a value with no case at run time is a runtime error (section 2).
- A `void` member of a tagged union is `Shape.empty` / `.empty` (section 4).
- The `Type_Info` tag numbers: `.ENUM` is 11, as Jai's, and `.TAGGED_UNION` is 32 (sections 1 and 4).
- Types where a value goes, and `#type` for procedure types (section 6).
- Kind tests through `#if #run type_info(T).tag == ...` (section 6).
- `Name(args).{...}` literals (section 7).
- The resizable-array procedures stay built in (section 10).
- All of Basic's maths goes generic; the old names go (section 10).
- The typed `less_*` procedures stay (section 10).

## Left to the builder

- How the checker stores instances, and how they're cached.
- Mangled names.
- How the parser tells `if x == {` apart from other `if`s, as long as the grammar in the spec is unambiguous.
- How `.NAME` gets its type from where it's used: an extra case in the same mechanism untyped `.{...}` literals use.
- The exact wording of messages not given above, in the spec's style.
- How each issue's tests are split between `tests/run`, `tests/errors` and the unit tests.
