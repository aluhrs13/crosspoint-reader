# Syncing the Intentional Fork

This repository intentionally carries behavior that is not expected to merge
upstream. Upstream updates should therefore be integrated through a reviewed
merge rather than by resetting `develop` to the upstream branch.

## One-time remote setup

```sh
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git
git remote -v
```

`origin` should point to this fork and `upstream` should point to the canonical
CrossPoint repository.

## Integrate an upstream update

Start with a clean working tree. If the clone is shallow, restore its history
before attempting the merge.

```sh
git fetch --unshallow origin  # shallow clones only
git fetch upstream master
git switch develop
git pull --ff-only origin develop
git switch -c sync/upstream-YYYY-MM-DD
git merge --no-ff upstream/master
```

Resolve conflicts on the sync branch, run the normal validation, and open a
pull request into `develop`. The merge commit keeps the upstream boundary
visible and makes the next sync easier to review.

## Fork-owned boundaries

Treat these areas as fork-owned when resolving conflicts:

- `src/ForkConfig.h` selects this fork's OTA release feed.
- `.github/workflows/release.yml` publishes the `firmware.bin` asset consumed
  by OTA updates.
- Fork-specific product features should remain in dedicated client, store, and
  activity modules with narrow hooks into shared CrossPoint code.

Preserve the following upstream-compatible contracts unless a migration is
deliberately designed:

- `firmware.bin` as the OTA asset name.
- `/.crosspoint/` paths and existing persisted JSON keys.
- NVS keys and the flash partition layout.
- Existing enum numeric values and binary cache versions.

## Validation

Run:

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
cmake -S test -B /tmp/crosspoint-tests -G Ninja
cmake --build /tmp/crosspoint-tests
ctest --test-dir /tmp/crosspoint-tests --output-on-failure
pio run
pio run -e gh_release
```

On hardware, confirm that the update screen reads releases from this fork and
monitor free heap during network and reader activity. Keep at least 50 KB free
under the expected worst-case workload.
