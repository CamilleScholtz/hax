---
name: downstream
description: Use when adding fork-specific behavior or integrating changes from OleksandrChekhovskyi/hax.
---

# Maintaining this downstream fork

This is a personal fork of https://github.com/OleksandrChekhovskyi/hax. It carries preferences
that may not fit upstream. Upstream acceptance is not a requirement for local features.

## Intended differences

- Up/Down move within the prompt's displayed rows, including soft wraps. Only Up on the first
  row or Down on the last row recalls history. Ctrl-P/Ctrl-N remain direct history shortcuts.
- The interactive prompt uses `>`, the shared spinner uses `| / - \`, and banner and
  submitted-prompt gutters use `|`. Markers use `>`, rendered bullets use `-`, and progress bars
  use `#` for fill and `.` for the empty track. This is a presentation preference, not a
  restriction on Unicode in prompts, model output, files, or other UI elements.

## Making changes

1. Inspect upstream's existing ownership boundaries and tests before editing. Change the module
   that owns the behavior rather than adding a parallel downstream implementation.
2. Keep each feature independent. Avoid unrelated formatting, renames, dependencies, and new
   configuration switches just to preserve upstream defaults in this fork.
3. Add regression tests at the lowest level that observes the difference. Follow `AGENTS.md`
   for formatting and validation; run `make tests` and `make lint`.
4. Record user-facing differences under `[Unreleased]` in `CHANGELOG.md`, marked `Downstream`,
   and update the intended differences above when they change.

## Integrating upstream

- Inspect remotes and the incoming diff; do not assume a remote name or branch. Fetching or
  changing shared state, merging, rebasing, and committing require the user's explicit request.
- Check whether upstream now supplies equivalent behavior. Prefer its implementation when it
  meets the local contract; remove redundant local code while retaining regression coverage.
- Resolve conflicts by preserving the intended behavior, not by keeping an entire local file.
  Pay particular attention to `src/terminal/input_core.c`, arrow dispatch in
  `src/terminal/input.c`, the prompt in `src/agent.c`, `src/banner.c`, and `src/render/spinner.c`.
- Run the full tests and lint after integration. Check multiline editing, history boundaries,
  and ASCII indicators in a dedicated tmux session as described in `AGENTS.md`.
- If upstream changes make a local feature costly to retain, explain the conflict and ask before
  dropping the preference or introducing a larger subsystem.
