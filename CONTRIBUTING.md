# Contributing

Read the [pinned engineering standard](infra/house/standard/README.md), whose `EN-` IDs
describe the common requirements. The [README](README.md) names the local gate,
test and lint commands. Run them against the final change and report what they
printed; an unavailable check is not a pass (EN-01.8).

Initialise pinned dependencies with `git submodule update --init --recursive`.
Develop on a branch and open a pull request; fork first if you lack write
access. Respect the protection on main. A branch with an issue begins
with its number, followed by a descriptive lowercase slug.
Each commit subject is `<area>: <summary>` within 72 characters. The area names
the changed directory; a repository-wide change names its dominant area or is
split. The summary is imperative, starts lowercase and has no final full stop.
The body explains the reason, wraps at 80 columns, and cites affected requirement
IDs. Every commit, including branch commits, carries no trailers or issue
references. Put issue references in the pull request description. A reviewed
change lands as one squash commit. The merger keeps its subject exactly the
pull request title, removing anything GitHub appends, and writes its body as the
change's argument (`EN-07.3`), with no issue reference (`EN-07.4`).

Warnings and lint findings block. A suppression names its check, covers one
site and has a preceding justification; scope disables and budgets require a
recorded deviation. Change behaviour, its specification and its tests together,
with stable requirement IDs. Keep documents about the current state. Source
files carry the SPDX identifier for the [licence](LICENSE).

Issue titles state an observable fact, without type prefixes. Discuss substantial
changes before implementation, and record deliberately deferred work in the
tracker. Security-sensitive reports follow [SECURITY.md](SECURITY.md).

Review refusal tests for the expected finding text as well as the exit status.
A crash returning the same status must never satisfy a negative control.
