# Datalog language reference

**Provenance:** recovered from `docs/language.html` at `30d4984^` (the docs-site
removal deleted it with the generated site) and converted to Markdown with
`pandoc -f html -t gfm`. Content is unchanged; only the page chrome was dropped.
This is the only prose description of the rule language, so it lives here rather
than in a generated site.


# Language Reference

This is the complete reference for the rule language. Every example is a real construct that the parser and compiler accept (several are drawn directly from the passing test suite). Rules are passed to the engine as source text via `dl_load_rules`, the CLI `query` command, or a `.dl` file.

## Comments

Line comments start with `#` and run to the end of the line.

``` code
# this is a comment
tc(X, Y) :- edge(X, Y).        # transitive closure base case
tc(X, Y) :- edge(X, Z), tc(Z, Y).
```

## Facts

A fact is a ground predicate with constants only — a predicate followed by a parenthesised argument list, terminated by a full stop. There is no `:-`.

``` code
edge(1, 2).
edge(2, 3).
person("alice").
```

## Rules

A rule is `head :- body1, body2, ..., bodyN.` The body atoms are conjoined with commas; the head is a single atom. Rules may reference existing relations (arity must match) or declare a new derived relation.

``` code
path(X, Y) :- edge(X, Y).
tc(X, Y) :- edge(X, Y).
tc(X, Y) :- edge(X, Z), tc(Z, Y).
```

Multiple rules with the *same head predicate* form a union: the engine materialises each rule and unions the results, deduplicating. There is no `;` disjunction operator — same-head rules are the way to express a disjunction.

``` code
p(X) :- edge(X, Y).
p(Y) :- edge(X, Y).   # p = union of the two rules' results, deduplicated
```

## Variables, constants, integers and strings

- **Variables** start with an uppercase letter `A-Z` or underscore `_`, then any alphanumeric or underscore.
- **Symbol constants** are bare lowercase identifiers — `foo`, `red`. They are interned to a symbol id.
- **String constants** are *double-quoted*: `"alice"`. These are interned symbol values, used anywhere a constant appears.
- **Integer constants** are decimal literals `0 1 2 ...`. Raw integer literals are limited to \< 2^31 (2147483648 is rejected) so they can never collide with list handles. `2147483647` is legal.

``` code
q("alice").          # double-quoted string constant
q(foo).              # bare lowercase symbol constant
q(42).               # integer literal (raw u32)
```

Note the asymmetry: `"foo"` (double quotes) is a string constant; `'...'` (single quotes) is a *regex pattern* and is only valid after `~` (see [Regex](#regex)). A bare lowercase identifier is itself a symbol constant.

## Recursion

Recursive rules are evaluated with a semi-naive fixpoint. A predicate is recursive if it appears in its own body, directly or through a cycle.

``` code
tc(X, Y) :- edge(X, Y).
tc(X, Y) :- edge(X, Z), tc(Z, Y).     # recursive: tc in its own body
```

Aggregates are *not* allowed in recursive rules (a compile error).

## Negation

A negated body atom is prefixed with `!`. Negation is *stratified*: a variable in a negated atom must be bound by a positive body atom first, and negation through recursion is rejected at compile time.

``` code
path(X, Y) :- edge(X, Y), !blocked(X, Y).
reachable(X) :- edge(X, Y), !blocked(X).
```

Un-stratifiable programs (a strict dependency cycle through negation) are a loud compile error.

## Equality

Body equality is written `X = Y` (both sides variables). It acts as a filter when both sides are bound, and binds an unbound variable from its bound counterpart otherwise.

``` code
q(X, Y) :- edge(X, Y), X = Y.        # keep rows where X == Y
q(Y)    :- edge(X, Y), X = Y.        # bind Y from an already-bound X
```

## Comparisons

Ordering comparisons `<` `<=` `>` `>=` accept a variable or an integer constant on either side. Inequality `!=` additionally accepts a symbol constant on the right (it interns the symbol and compares symbol ids). A symbol constant is *not* allowed in an ordering comparison.

``` code
lt(X, Y) :- pair(X, Y), X < Y.
le(X, Y) :- pair(X, Y), X <= Y.
gt(X, Y) :- pair(X, Y), X > Y.
ge(X, Y) :- pair(X, Y), X >= Y.
ne(X, Y) :- pair(X, Y), X != Y.
r(X)     :- val(X), X != foo.        # != accepts a symbol-constant RHS
```

An ungrounded comparison operand (a variable not bound by any positive body atom) is a loud compile error.

Comparisons are only meaningful over integers. An ordering comparison over a variable that holds *symbols* is a loud compile error (symbol ids are interned handles and carry no order); the same holds for a `!=`/`=` between a symbol variable and an integer variable. The symbol-constant right-hand side of `X != foo` is the documented exception: it interns the constant and compares symbol ids on any column — including an integer column, where a small symbol id can equal a raw integer value, so the answer depends on intern order. Variables produced by aggregates and arithmetic hold integers (`N = count()`, `N = X + 1`) and may be compared freely; `min`/`max` pass the source column's kind through. The result variables of the string, list and range builtins carry kinds too (see [String builtins](#strings), [Lists](#lists) and [Range](#range)). A list result is also rejected as an arithmetic operand — a term handle has no numeric value.

## Arithmetic

Arithmetic is written `X = <expr>` in the body, producing the value `X`. Operators are `+ - * / %`; `* / %` bind tighter than `+ -`, all left-associative, with parentheses allowed. Operands are variables and integer constants (a symbol constant is rejected — symbols have no numeric value). Arithmetic wraps at u32 (`0 - 1 == 0xFFFFFFFF`), and division/modulo by a literal `0` is a compile error (a variable divisor of 0 simply yields no tuple).

``` code
r1(X) :- pair(A, B, C), X = A + B * C.      # precedence: A + (B*C)
r2(X) :- pair(A, B, C), X = (A + B) * C.    # parentheses
add(X, Y, S) :- pair(X, Y), S = X + Y.
inc(X)       :- pair(A, B, C), X = A + 1.   # integer constant operand
```

Arithmetic in a recursive rule works (bounded by the fixpoint), and the result variable must not be bound before the arithmetic is evaluated.

## Aggregates

Aggregates appear *in the body*, binding a result variable: `N = count()`, `S = sum(Y)`, `M = min(Y)`, `M = max(Y)`. The result variable is referenced in the head, and every *other* head variable is the implicit group-by key.

``` code
# group by X: number of outgoing edges per node
cnt(X, N) :- edge(X, Y), N = count().

# group by X: total of Y per X
total(X, S) :- edge(X, Y), S = sum(Y).

# min / max of Y per X
minv(X, M) :- edge(X, Y), M = min(Y).
maxv(X, M) :- edge(X, Y), M = max(Y).

# no group-by vars -> a single global count
cnt(N) :- edge(X, Y), N = count().
```

Constraints on aggregates:

- `count()` takes no arguments; `sum/min/max` take exactly one variable argument.
- Only one aggregate is allowed per rule.
- Aggregates are not allowed inside negation, in recursive rules, or in rules whose head contains a constant.
- Aggregates over a variadic relation are rejected.
- The aggregate result holds an integer for `count`/`sum` and the source column's kind for `min`/`max`; a downstream join or comparison that mixes it with the other kind is a compile error.

## String builtins

String values are interned symbols. Two kinds of builtin exist:

- **Producers** (bind a result variable): `C = concat(A, B)`, `N = length(S)`, `L = lower(S)`, `U = upper(S)`.
- **Filters** (test a string): `prefix(S, P)`, `suffix(S, P)`, `contains(S, P)`.

``` code
cat(A, B, C) :- pair(A, B), C = concat(A, B).
lens(A, N)   :- str(A),    N = length(A).       # byte length
lc(X)        :- s(Y),      X = lower(Y).        # ASCII case folding
uc(X)        :- s(Y),      X = upper(Y).
pref(X)      :- str(X),    prefix(X, "he").
suf(X)       :- str(X),    suffix(X, "o").
cont(X)      :- str(X),    contains(X, "ll").
```

`lower` and `upper` are implemented (ASCII A–Z folding). `length` is the byte length (UTF-8 bytes, not codepoints), and it also accepts a constant list literal (list length). String operands must be variables or double-quoted string constants — a raw integer operand is rejected. A producer result longer than 4096 bytes backtracks (no tuple).

A producer’s *result* carries a kind. `concat`, `lower` and `upper` yield a symbol (an interned string id): an ordering comparison over their result is a loud compile error (symbol ids carry no order), as is joining it against an integer column (a symbol column and an integer column are different value spaces). `length` yields an integer (a byte count) and may be compared and joined freely. Only the result is typed — the *input* operand is unaffected (a symbol variable fed to `length` stays valid).

## Lists

Lists are first-class values, interned in a term store (equal lists share one handle). List literals are written `[e1, e2, ...]`; `[]` is the empty list.

- **Producers**: `L = cons(H, T)`, `H = car(L)`, `T = cdr(L)`, `L = append(A, B)`.
- **Filter / generator**: `member(X, L)` — tests X when X is bound, enumerates L’s elements when X is unbound.
- **Patterns**: `[X | Xs]` destructures a list into head and tail; it can appear as a relational argument (e.g. `p([H | T])`) or as a list-assignment form `[H | T] = L`.

``` code
r(X, H, T, A) :- p(X), L = cons(X, [7, 8]), H = car(L),
                 T = cdr(L), A = append(L, [9]).
q(H, T)       :- p([H | T]).                 # pattern in a relational arg
r(X)          :- p([7, X]).                  # constant element + var
r(X)          :- p(L), member(X, L).         # generator
s(X)          :- num(X), p(L), member(X, L). # filter
empty_tail(A, B) :- [A, B] = [1, 2].         # assignment form, empty tail
tail_pat(H, T)   :- [H | T] = [1, 2, 3].     # assignment form, tail
from_var(H, T)   :- p(L), [H | T] = L.       # assignment from a var
```

A list pattern is not allowed in a rule head or fact (use `cons` to build), or in a negated atom (patterns cannot bind variables under negation). The tail after `|` must be a variable.

`cons`, `cdr` and `append` return a list (a term handle): an ordering comparison over their result is a loud compile error (handles have no order), and so is arithmetic over it. Joining such a result against an integer or symbol column, or using it in `!=`, stays valid — a handle never collides with a small raw integer or symbol id. `car` is different: it returns the head *element*, not a handle, so its result kind is that of the element (an integer, a symbol, or a nested list) and is not known statically — comparisons over a `car` result are unchecked.

As a generator, `member(X, L)` binds X to L’s elements. When L is a constant literal whose elements are all integers (or all symbols), X carries that kind and an ordering comparison over it is checked like any other variable. When L is a variable or holds mixed elements, X’s kind is not known statically and comparisons over it are unchecked.

## Range

`range(X, Rel, Lo, Hi)` is a reserved builtin that scans the *distinct leading-column values* of the relation `Rel` in the half-open interval `[Lo, Hi)`:

- `X` is the variable, **first**.
- `Rel` is the relation *name* (an identifier, not a variable).
- `Lo` and `Hi` are the half-open bounds (variable or integer constant).

When `X` is unbound, `range` acts as a generator yielding each distinct `col0` value of `Rel` in `[Lo, Hi)`, in lex order. When `X` is bound it acts as a filter. The range is over the leading column only.

``` code
q(X) :- range(X, r, 10, 20).          # X in {col0 of r in [10, 20)}
p(X) :- r(X), range(X, r, 10, 20).    # filter form
q(X, Lo, Hi) :- bounds(Lo, Hi), range(X, r, Lo, Hi).   # var bounds
```

`Rel` must be a known, non-variadic relation of arity ≥ 1. Range over a recursive relation, an unknown relation, a negated range, or an ungrounded bound variable is a loud compile error.

The result variable takes the *kind of the named relation’s leading column*: over an integer column a comparison over the range variable is meaningful, while over a symbol column an ordering comparison is a loud compile error. When the relation’s leading column has no established kind (a fresh or empty relation), the result stays untyped and comparisons over it are unchecked.

## Regex patterns

A body atom may carry a regex pattern written as a *single-quoted* string after a tilde: `pred(...) ~ 'pattern'` or, to target a specific column, `pred(...) ~ k 'pattern'` where `k` is a 0-based integer column index (default 0 = leading column). The pattern is compiled to a DFA and matched against the *string content* of that column's value (via the symbols table), not the raw binary key — so `~ 'a.*'` means "column's text starts with `a`". An integer (non-string) column matches nothing. In the CLI, use `pattern <rel> [<col>] '<pattern>'` for standalone regex queries.

``` code
q(X, Y) :- edge(X, Y) ~ 1 '(a|b).*'.   # filter on column 1's text
```

A negated pattern atom is not supported (compile error). A column index out of range for the atom is also a compile error. In rules, patterns are compiled at load time; a bad regex is a loud error.

Patterns match a **whole symbol name**, so `~ 'a.*'` and `~ '^a.*'` are the same thing. `^` as the pattern's first character and `$` as its last are accepted anchors that constrain nothing extra; an anchor anywhere else (`a^b`, `a$b`) is a compile-time error rather than a silent non-match. `\^` and `\$` are the literal characters.

## Variadic relations

A relation may be declared *variadic*, accepting facts of any arity 1–8. Storage is per-arity fixed-width; rule atoms resolve to the variant matching their syntactic argument count. A variadic head must be declared before `dl_load_rules`. Aggregates over a variadic relation and recursive variadic heads are rejected at compile time (such programs always evaluate via the full fixpoint).

``` code
/* C API: */
dl_declare_relation_variadic(db, "v");
```

## Reserved names

The builtin names are reserved — a rule head cannot use them (rejected with a clear diagnostic): `member`, `car`, `cons`, `cdr`, `append`, `concat`, `length`, `lower`, `upper`, `prefix`, `suffix`, `contains`, `range`.

## Compile-error semantics

The engine never silently mis-evaluates. Unsupported or malformed programs are rejected with a diagnostic at *compile time* and `dl_load_rules` / `dl_compile` return -1. Rejected cases include:

- unstratifiable negation or an unstratifiable strict cycle through range;
- unsafe negation (a variable not bound by a positive atom);
- ungrounded comparison, arithmetic, string, list, member, or range operands;
- division/modulo by a literal `0`;
- aggregates in recursive rules, two aggregates, negated aggregates, constants in an aggregate head;
- list patterns in heads/facts or negated atoms; a reserved name as a head;
- range over a recursive / unknown / variadic relation;
- integer literals ≥ 2^31; a symbol constant in an ordering comparison or arithmetic;
- a rule with no positive body atom (unless a lone `member`/`range`/list-assignment drives it).

