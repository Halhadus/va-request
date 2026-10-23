# Tested hardware

Reports describe the kernel interface and tests available on a particular system. Results apply to the recorded kernel and source revision.

| Platform | Report | Evidence |
| --- | --- | --- |
| RK3588 | [Hardware, vendor and kernel capabilities](rk3588/README.md); [format/control inventory](rk3588/inventory.md) | Run `run-stph_nco`: decode, JPEG, VPP, DMA-BUF and renderer checks passed; scope and skips are recorded in the report. |

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
