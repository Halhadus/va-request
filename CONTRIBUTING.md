# Contributing

Use an issue for reproducible bugs, feature proposals or hardware reports. Changes are reviewed through pull requests against `main`.

## Reports

A bug report needs the source or package version, device/SoC, distribution, kernel release, relevant drivers and an exact reproduction. Include application versions, codec/profile, dimensions and a sample where possible. For regressions, include the last working source and kernel revisions.

Attach the `REPORT=...tar.gz` archive produced by `tools/test.py`. Hardware reports also need the JSON inventory from `tools/collect_capabilities.py`; `v4l-utils` provides its `v4l2-ctl` dependency. The [testing guide](docs/testing.md) lists commands, dependencies and coverage. Include collection errors when a report is incomplete.

Use the Hardware report form to propose a device entry. Report working, failing and untested operations separately. The maintainer reviews the evidence before adding results under `docs/hardware/<device>/`. See the [hardware report requirements](docs/hardware/README.md).

## Changes

Keep each pull request focused on one behavior or related feature. Explain the trigger, resulting behavior and validation. Avoid unrelated formatting changes. Preserve license notices and credit code imported from other projects.

Shared V4L2 formats, controls and queue semantics belong in the common implementation. A compatibility module needs a narrowly identified implementation, an observed exception and regression coverage. Stateful event/drain semantics and stateless Request semantics belong in their respective common backends. New codecs or layouts need capability checks, parameter/layout validation, ownership handling and tests.

## Validation

Run `python3 tools/test.py --suite unit` before submitting code. Use `--sanitize` with a separate build directory for sanitizer checks. Changes to memory, queues, codec translation or processing also need results from the affected hardware. Include untested cases in the PR description.

CI runs normal and sanitizer unit suites and builds the source package. Hardware runs are submitted separately or started manually on a configured test runner.

## Review and merging

PRs need passing required CI checks, a code-owner review and resolved review conversations. Keep changes in reviewable commits; squash and rebase merges are enabled. Maintainers may request further hardware evidence before merging.

Issues start with `needs-triage`. Maintainers use `needs-information` or `needs-hardware-test` for missing evidence, and `regression` for behavior confirmed to have worked previously. `hardware-report` identifies device qualification reports.
