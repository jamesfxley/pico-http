# pico-http documentation

| Document | What it is | Read it when |
|---|---|---|
| [traceability-exceptions.txt](traceability-exceptions.txt) | Requirements no source file can cite (EN-02.7). | Reviewing uncitable requirements. |
| [orientation.md](orientation.md) | How this repository is built and gated, how it is laid out, and the repository's invariants and traps | Working in it for the first time, or before an edit |
| [technical-specification.md](technical-specification.md) | Normative. Scope, platform constraints, resource budgets, and every requirement (`AR`/`PR`/`RR`/`BR`/`TR`/`NR`/`OR`) with its pass criterion. | Deciding what the library must do, or citing a requirement from code |
| [architecture.md](architecture.md) | How the implementation works, layer by layer: modules, connection identity, the RX and TX paths, timeouts, teardown. | Changing the internals |
| [usage-guide.md](usage-guide.md) | Practical usage: setup, routes, the three body modes, framing, limits, host testing. | Building an application on the library |
| [future-work.md](future-work.md) | Work deliberately not done, one entry per deferral, with its reason and cost (`EN-03.7`) | Asking whether something absent is planned, or before proposing a feature |
| [deviations.md](deviations.md) | Standing (scope-wide) deviations from the coding standard, each with a rationale, a risk assessment and a review date | Adding or reviewing a suppression |
| [deviation-budget.txt](deviation-budget.txt) | One suppression ceiling per language, governed by EN-00.6 | Same |
| [requirement-prefixes.txt](requirement-prefixes.txt) | The pinned prefix set, checked by review (EN-02.4) | Adding a requirement under a new prefix |
| [GitHub Releases](https://github.com/jamesfxley/pico-http/releases) | What changed in each release, and what to change in your code when an API broke | Upgrading across a release |

The API reference is generated from header declarations:

```sh
doxygen docs/Doxyfile      # -> docs/api/html/index.html (gitignored)
```

CI builds the reference on pushes to `main` and pull requests, failing on
undocumented public entities. The [porting
guide](usage-guide.md#porting-a-custom-transport-or-profile) covers custom
transports and sizing profiles. `tests/docs/` compiles the README and
usage-guide C++ examples directly from Markdown.

Device run records and their conditions belong beside the
[benchmark](../examples/bench/baselines/). Reference records are deferred; see
[future work](future-work.md).

The specification is the only normative document here. `architecture.md`
describes what is, and work deliberately not done is in
[future-work.md](future-work.md) (`EN-03.7`); the issue tracker holds defects.

A behaviour claimed in `README.md` or `docs/orientation.md` but absent from the
specification is not a commitment, and a requirement is not satisfied until the
matrix names what establishes it.

## Conventions

Requirement citations follow
[EN-02.1 to EN-02.3](../infra/house/standard/engineering/02-spec-and-traceability.md).
Specification content and measurements follow
[EN-03.2](../infra/house/standard/engineering/03-documentation.md).

- A behaviour change lands as one change: the specification row, a host test,
  and the affected documents together.
