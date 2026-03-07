---
inclusion: fileMatch
fileMatchPattern: ".kiro/steering/*.md"
---

# Steering Principles

Every rule in a steering file must satisfy these principles. When adding or modifying rules, self-check each one against this list before committing.

## 1. Structured

Rules are grouped by topic with clear headings. Related rules stay together. Use tables for pattern/example pairs, bullet lists for constraints, and code blocks for templates. No wall-of-text paragraphs mixing multiple concerns.

## 2. Deterministic

Each rule has exactly one interpretation. Avoid subjective qualifiers (`prefer`, `keep short`, `non-obvious`, `concise`). Instead, state the exact condition and the exact action:
- Bad: "Prefer fixed-width types"
- Good: "Use fixed-width types (`int32_t`, `uint8_t`). Exception: function return values, loop counters, and error codes may use `int`."

## 3. Triggerable

A rule is triggerable if an AI agent can detect a violation by inspecting the code. Every rule must answer: "Given a file, what pattern do I search for, and what constitutes a violation?"
- Bad: "Write clean code"
- Good: "All `if`/`else`/`for`/`while` bodies must use braces, even single statements"

## 4. Unambiguous

No two rules may conflict. If a rule has exceptions, list them explicitly in the same section. Examples in rules must comply with all other rules in the same file (e.g., a code example in the Comments section must also follow the Naming Convention).

## 5. Scoped

Each rule states what it applies to: file types, function categories, or specific contexts. A rule without a scope is assumed to apply everywhere, which is rarely correct.
- Bad: "Use `/** ... */` for comments"
- Good: "Use `/** ... */` for multi-line comments in `.c` files. Use `/* ... */` for single-line."

## 6. Exemplified

Rules with patterns or naming conventions must include at least one positive example (correct) and, when the violation is common, one negative example (incorrect). Examples are the fastest way to eliminate ambiguity.

## 7. Minimal

Do not add rules that duplicate language defaults, compiler behavior, or formatter output. Only document what cannot be inferred from the toolchain. If `clang-format` already enforces a style, do not restate it unless the rule adds constraints beyond what the formatter handles.

## 8. Versioned

When a rule changes (renamed pattern, new exception), update all examples and references in the same commit. Stale examples are worse than no examples — they teach the wrong pattern.
