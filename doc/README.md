---
title: Diluvium documentation
description: The reference for the language, the runtime, and embedding both.
---

# Diluvium documentation

Diluvium is a 100% backward-compatible extension of Lua 5.5: string
interpolation, regular expressions, null coalescing, `switch`, `continue`,
`defer`, secure functions, and typed numeric arrays whose float results are
bit-identical on every target. Under 1 MiB, and it runs just about
everywhere.

[Try it in the browser](https://diluvium.aloecraft.org) · [Install](https://diluvium.aloecraft.org/start) · [Releases](https://software.aloecraft.org/releases/diluvium/)

```sh
curl -fsSL https://diluvium.aloecraft.org/start | sh
```

## The language

- **[Programmer's Guide](Guide.md)** — the one to read first. Every feature
  the language adds, with the Lua it stays compatible with.
- **[Numeric spec](diluvium-numeric-spec.md)** — the typed arrays, and what
  "bit-identical on every target" is a promise about.
- **[Determinism](Determinism.md)** — what is reproducible, and what is not.
- **[Analyzer](Analyzer.md)** — the static checks, and how to run them.

## Embedding and extending

- **[Host](Host.md)** — embedding the interpreter in a C program.
- **[Hostcall](Hostcall.md)** — the call boundary in both directions.
- **[Extending](Extending.md)** — adding libraries.
- **[Capabilities](Capabilities.md)** — what a chunk is allowed to reach.
- **[Hibernate](Hibernate.md)** — suspending and resuming a running state.

## DRT, the runtime

- **[DRT](DRT.md)** — the runtime that shares the landing page.
- **[Messaging](Messaging.md)** — the connector protocol.

## Around the language

- **[Lab](Lab.md)** — the browser playground at
  [diluvium.aloecraft.org/lab/](https://diluvium.aloecraft.org/lab/).
- **[Benchmarks](Benchmarks.md)** — what is measured, and on what.

---

Working documents in this directory — `ROADMAP.md`, `Plan-2026-09.md`,
`ALIGNMENT.md`, the `BUILD*.md` notes, `audit/` and `attic/` — are the
project's own notes rather than a reference, and are deliberately not
published to the documentation site. `docsite/docusaurus.config.js` carries
the allowlist that decides.
