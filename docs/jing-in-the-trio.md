# Jing's design decisions, in service of the trio

**Status:** Why-note (2026-08-17). Explains the *purpose* of the datalog-dafsa fact store (jing) as the grounding tier of a larger three-treasure reasoning system — shen / jing / qi — rather than as a standalone search stack. This is the "why" that makes the individual design docs cohere. **This note is self-contained: the trio context is summarized inline below; the canonical vision doc lives outside this repo and is not required reading here.**

## The reframe

datalog-dafsa is **jing** — the recorded essence — one of three treasures:

- **shen** — the spirit: a sequent-calculus-based compiler/meta-interpreter (a Lisp whose type system is founded on sequent calculus — typing judgments are sequents). The formal substrate. Checks **structure** (cut elimination, subformula property).
- **jing** — the essence: the fact store (datalog-dafsa). Records and grounds knowledge. Checks **grounding** (premises are real / derivable).
- **qi** — the vital energy: a specially trained LLM that reasons *in shen* (producing sequent derivations). Does **generation**.

### The vision, in brief

**The LLM proposes, the Hauptsatz (cut elimination) disposes.** Qi is trained to *produce* shen sequent proofs; shen then *attempts cut elimination* on them. The cut rule lets a derivation import an intermediate formula A:

```
Γ ⊢ A     Δ, A ⊢ B
──────────────────
   Γ, Δ ⊢ B
```

By Gentzen's Hauptsatz, any proof with cuts normalizes to a **cut-free** one, and cut-free proofs have the **subformula property** (nothing foreign is introduced). This is the falsification criterion:

- **Normalizes to cut-free** ⟹ reasoning is *analytic* — everything used was latent in the premises. Grounded.
- **Cannot eliminate a cut** ⟹ the intermediate A is a **smuggled assumption** doing unjustified work — the reasoning is *falsified* (Popperian).

This attacks exactly what LLMs are worst at: **unstated premises.** It checks whether the *derivation* is structurally honest, not whether the conclusion "looks right."

**Jing's role in the trio:** the vision notes cut elimination is *necessary but not sufficient* — a cut-free proof can still be about the wrong thing (sound within assumed axioms, not true in the world). Jing is the **second tier that grounds the premises**: it checks that each premise is an actual recorded fact or Datalog-derivable from the fact base. So the trio is a **reasoning-honesty engine** (falsifies structural cheating + ungrounded premises), not a truth engine.

### The one-line summary of the vision

> A neuro-symbolic reasoning system where the LLM proposes and the Hauptsatz disposes — falsification + grounding baked into how reasoning is structured, not applied post-hoc.

## The core synthesis: analyticity at two levels

The reason the trio coheres is that two of its members enforce the *same* epistemic discipline at different levels:

> **"Nothing foreign introduced; everything derived was latent."**
> - **shen, intensionally:** cut-free proofs have the subformula property — every formula is a subformula of the conclusion. An ineliminable cut is a smuggled assumption.
> - **jing, extensionally:** the Datalog VM only derives relations that are entailed by stored facts (IVM-maintained). It cannot conjure a fact that isn't latent.

So a derivation is honest only if it survives **both** checks: shen's cut elimination (structure) *and* jing's derivability/grounding (content). The trio is a **reasoning-honesty engine**, not a truth engine.

## How each jing design decision serves the trio

| Design decision | Why it matters for the trio |
|---|---|
| **One append-only sym-id space** (`intern.h`) | Every fact/entity/edge is keyed by a stable, never-reused `sym_id`. This is the join key that lets qi's premises (names) map deterministically onto jing's facts — the substrate for the premise↔fact bridge. |
| **Datalog-composable relations** (entity/edge/observation as rules) | The VM can *derive* whether a premise is entailed by the facts. "Is this assumption actually supported?" = a Datalog query. This is jing's grounding check, mechanized. |
| **Co-versioned snapshots + `dl_query_version`** | Reasoning can be **re-audited as-of the exact fact-state it was grounded on.** "Was this premise supported when this argument was made?" jing keeps the *history of premises*. This is a capability most reasoning systems lack. |
| **IVM (incremental view maintenance)** | Derived relations stay current as facts change — grounding never goes stale. When qi reasons, jing's "is this derivable?" answers reflect the live fact base. |
| **Time-travel / as-of queries** | Enables the re-audit story above; also means the fact base can be examined at any prior point for post-hoc analysis of a reasoning trace. |
| **The one-embed-pass + atomic publish** (vector/no-sidecar docs) | The semantic + lexical tiers are co-published with the facts, so a premise's *semantic* support (not just symbolic) is grounded against the same consistent snapshot. |

## What the trio gives jing in return

The direction isn't one-way. Being the grounding tier of shen/qi raises jing's bar:
- Premises must be **queryable as facts** and **derivable by Datalog** — which is why Datalog-composability was worth building.
- Reasoning must be **re-auditable as-of time** — which is why snapshots/time-travel are load-bearing, not optional.
- Semantic support must be **co-versioned with the facts** — which is why the embed pass and vector tier are atomic with publish.

## The two open blockers (the honest joints)

1. **The premise↔fact-store query bridge.** Shen is intensional (a Lisp/calculus); jing is extensional (relational sym-ids + Datalog). Mapping "a sequent premise" to "a fact-store query / derivability check" is the untested seam. The sym-id space is the substrate; the *semantic* mapping is not built.
2. **The qi training signal.** RL with cut-elimination-success as reward vs supervised on normalization traces. With jing present, the sharper target is **"proof normalizes AND its leaves are facts/derivable in jing"** — grounded cut elimination is a richer reward than structural alone.

## The one-line summary

> **jing is the extensional conscience of a three-treasure reasoning system:** the fact store whose Datalog derivability, co-versioned snapshots, and sym-id space give qi's reasoning both *grounding* and *re-auditability* — the content half of the same "nothing foreign" discipline that shen's cut elimination enforces on the structure half.

## References

- The trio vision is summarized inline above (this note is self-contained; the canonical vision doc lives in a sibling project outside this repo).
- The design docs this draws on: `docs/datalog-dafsa-search-stack.md` (sym-id space / Datalog-composability / time-travel synthesis), `docs/datalog-dafsa-vector-search.md` + `-no-sidecar.md` (atomic co-published tiers), `docs/datalog-dafsa-cas.md` (write-path), `src/intern.h` / `src/dl.h` (snapshots, IVM, Datalog VM).
