# Project workflow

- The user cancelled the Judge workflow. Do not use it unless requested again.
- Do not dispatch or re-enable CI/CD workflows. Build and test locally.
- Keep compilation, correctness checks and measured performance results
  separate. Passing a build does not establish an FPS gain.
- The user's daily phone is for occasional final validation. Use existing
  private captures and PC tests for investigation and iteration first; do not
  request a device session for each speculative change or new APK.
- Before requesting phone time, complete offline analysis, state the causal
  hypothesis and expected observable result, and prepare a bounded automated
  comparison with restoration. Desktop FPS and synthetic pixel tests cannot
  establish Android/Adreno game performance. Current timing traces are not a
  complete graphics replay capture.
- Respect each agreed phone window and restore before its deadline. Do not
  silently extend it or retry indefinitely after a harness failure.
- Barcelona Slot 1 is load-only: never save over it. Keep game/device assets,
  raw captures and phone harnesses private.
