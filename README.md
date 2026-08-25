# Blackjack solver & GameplayKit RNG study

A small course project in two parts:

1. **An exact optimal-play solver for a 21 / blackjack variant** (`blackjack21_exact_solver.cpp`)
   — a parallel dynamic-programming search that, given a fixed deck order, returns
   the provably highest-scoring sequence of plays. Good practice with memoization,
   hashing, and lock-free concurrency.

2. **Reproducing Apple's GameplayKit RNG on other platforms.** `GKMersenneTwisterRandomSource`
   only runs on macOS, so the Swift programs here dump its exact output for known
   seeds. The point of the exercise is to study a platform-specific RNG and
   rebuild its sequence in portable code (Python) that runs anywhere.

## Files

- `blackjack21_exact_solver.cpp` — exact optimal 21-blackjack DP solver (OpenMP).
- `bj_test_deck.txt`, `bj_test_deck_slow.txt` — sample decks with known best scores, for tests.
- `generate_vectors.swift` — dumps GameplayKit RNG output for each seed.
- `uniforms.swift` — emits the `nextUniform()` bit-pattern sequence for one seed.
- `sub_seeds.swift` — emits a few derived integer sub-seeds for one seed.
- `seeds.json` — the seeds to test.
- `inspect_vectors.py` — local sanity check for a downloaded vectors file.
- `.github/workflows/` — build/run the C++ and Swift on CI and upload results as artifacts.

## Notes

Swift runs on a GitHub Actions **macOS** runner (GameplayKit is Apple-only); the
C++ solver builds on a Windows runner. Each workflow uploads its result as an
artifact you can download from the **Actions** tab. Workflows are triggered
manually (`workflow_dispatch`) or, for the solver build, on changes to the source.
