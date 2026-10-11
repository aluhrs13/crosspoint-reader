"""
PlatformIO pre-build script: apply CrossPoint's FreeInk UI patches via `git apply`.

`ListItem` lives in the freeink-sdk submodule. A strikethrough flag is needed
for the Readwise Queued tab, and the submodule pin is not moved: CI fetches
the published SHA, so the change is carried here and applied to the working
tree before compile. `git apply --check --reverse` makes a second build a
no-op. The parent gitlink stays on the upstream commit.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys


PATCH_DIR = os.path.join(env["PROJECT_DIR"], "scripts", "freeink_patches")  # noqa: F821


def patch_freeink_ui(env):
    sdk_dir = os.path.join(env["PROJECT_DIR"], "freeink-sdk")
    if not os.path.isdir(os.path.join(sdk_dir, ".git")) and not os.path.isfile(
        os.path.join(sdk_dir, ".git")
    ):
        raise SystemExit("ERROR: freeink-sdk is not checked out; cannot apply UI patches")
    for patch in _patch_files():
        _apply_one(sdk_dir, patch)


def _patch_files():
    if not os.path.isdir(PATCH_DIR):
        raise RuntimeError(
            "FreeInk UI patches missing -- aborting build (expected directory %s)"
            % PATCH_DIR
        )
    patches = sorted(
        os.path.join(PATCH_DIR, name)
        for name in os.listdir(PATCH_DIR)
        if name.endswith(".patch")
    )
    if not patches:
        raise RuntimeError(
            "FreeInk UI patches missing -- aborting build (no .patch files in %s)"
            % PATCH_DIR
        )
    return patches


def _apply_one(sdk_dir, patch_path):
    name = os.path.basename(patch_path)
    if _git_apply_succeeds(sdk_dir, patch_path, reverse=True):
        return
    if not _git_apply_succeeds(sdk_dir, patch_path, reverse=False):
        result = subprocess.run(
            ["git", "apply", "--check", patch_path],
            cwd=sdk_dir,
            capture_output=True,
            text=True,
        )
        sys.stderr.write(
            "ERROR: FreeInk UI patch %s does not apply cleanly:\n%s%s\n"
            % (name, result.stdout, result.stderr)
        )
        raise SystemExit(1)
    subprocess.run(["git", "apply", patch_path], cwd=sdk_dir, check=True)
    print("Applied FreeInk UI patch: %s" % name)


def _git_apply_succeeds(sdk_dir, patch_path, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append(patch_path)
    return subprocess.run(
        cmd, cwd=sdk_dir, capture_output=True, text=True
    ).returncode == 0


patch_freeink_ui(env)  # noqa: F821
