# Tested hardware

Reports describe the kernel interface and tests available on a particular system. Results apply to the recorded kernel and source revision.

| Platform | Results | Status |
| --- | --- | --- |
| RK3588 | [Device report](rk3588/README.md); [kernel inventory](rk3588/inventory.md) | Stateless decode, JPEG, VPP, DMA-BUF and renderer checks passed. |
| POCO F5 | [Device report](poco-f5/README.md) | Native musl unit and renderer checks passed. Stateful decode failures remain; native decoder DMA-BUF is unqualified. |

Each device page summarizes its latest tests. Replace results when a newer run covers the same operations; keep report archives on the issue or CI run. A unit-only run does not replace hardware results.

## Submit a hardware report

Open the Hardware report Issue form. It applies the `hardware-report` label and requests:

- Device model and SoC; distribution and architecture.
- Kernel release, source branch, custom patches, V4L2 drivers and DRM driver.
- Driver commit or package version and local changes.
- Capability inventory JSON from `tools/collect_capabilities.py`.
- The test archive printed as `REPORT=...` by `tools/test.py`, with its command and render node.
- Tested codecs/profiles, bit depths, dimensions, encode/processing operations and applications; failures and untested paths.

The inventory command is read-only:

```sh
python3 tools/collect_capabilities.py --output verify/capabilities/report.json
```

Results are reviewed against the exact source and kernel before a device entry is added. A device directory contains `README.md` with the test scope and results, plus an inventory where useful. Keep report archives attached to the issue or CI run.

Use PASS, FAIL and SKIP results with their recorded scope. A format or control in the kernel inventory is an advertised capability; working operation requires a completed test. Reports should distinguish verified operations, known failures and untested paths.
