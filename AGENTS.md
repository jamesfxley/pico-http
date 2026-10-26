# pico-http

This repository is a C++20 HTTP server library of the pico family for RP2350 CYW43 firmware and portable host tests. Its
`house.toml` selects the pinned platform and package modes. Read README.md,
CONTRIBUTING.md and docs/README.md before work.

Done means `python3 dev.py check` passes against the final state and the final
diff is reviewed. Use `py -3 dev.py check` on Windows where installed.
`python3 dev.py check --list` names applicable gates; `--all` adds online checks.
Warnings fail. Report unavailable checks as missing evidence, never as passes.

Before work, read `infra/agents/AGENTS.md`; it routes to the family's and the platform's guides.

Keep family facts here. Machine notes live in `local.md` in your House config
directory; `python3 dev.py context` prints where that is. Extend the platform
only through its declared configuration and `house.api`; do not edit pinned or
synced files. Initialize recursive submodules before checks.
Use pinned guide links, not installed copies. Document public installed module
targets separately from internal exports. Commit subjects name the changed
directory or dominant area; no trailers. The working agreements' approval
boundaries apply.

never create `CLAUDE.md` or `CLAUDE.local.md`
