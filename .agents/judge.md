## 2026-10-11 — Author-engine fallback needs an independent reviewer

**Scope:** Perf24 queued surface-copy eligibility and diagnostics.
**Preflight:** Codex CLI 0.162.0-alpha.2 is available; Claude and agy are absent
from PATH and the documented user install fallbacks.
**Review status:** Not run. The available engine authored the change, and the
skill requires a different model before a single-engine review can proceed.
The user has a pending choice between GPT-6 Astra and configuring Claude.
**Insight:** Record preflight separately from executed review evidence; never
turn successful pixel fixtures or an APK build into a Judge approval.
**Apply when:** The only available review engine also generated the code.

## 2026-10-11 — Perf25 presentation candidate prepared locally

**Scope:** Opt-in Android native request cadence, timing regression checks and
a separate locally signed debug application ID. Approximate review unit is
150 lines including documentation; perf24 remains a separate review unit.
**Review status:** Not run. The independent-model choice remains unanswered.
The candidate is uncommitted and has no device performance/compatibility
verdict. Host timing checks, local APK build and payload signature verification
pass; the ARM64 timing target compiles. Those checks are not a Judge verdict.
**Insight:** Profiling shows repeated scanout requests and stationary slowdown
with zero texture decoding, but does not prove identical images or causality.
**Apply when:** A performance candidate can be prepared and checked offline
while keeping deployment and independent-review evidence explicit.

## 2026-10-11 — Workflow cancelled by the user

The user explicitly said to forget Judge and advance. The requirement and
pending reviewer-model choice are withdrawn. No Judge review was executed;
continue ordinary local build/correctness/performance work without that gate.
