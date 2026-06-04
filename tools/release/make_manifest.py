"""Generate a release manifest.json from pre-built binaries.

This script is invoked by .github/workflows/release.yml after the ESP-IDF builds
have completed. It does NOT build itself — that's CI's job (it has the IDF setup).
For local use, run idf.py build for both apps first.

Schema is defined in ota_design.md §5.

Channel derivation:
    v1.2.3         -> stable
    v1.2.3-rc.N    -> beta
    v1.2.3-beta.N  -> beta
    --channel dev  -> dev (free-form --version, no semver enforcement;
                      used by release-dev.yml to ship branch builds as
                      dev-<shortsha>-<utcstamp>. Recovery-only consumption.)

Usage:
    python make_manifest.py \\
        --version v0.1.0-rc.1 \\
        --app-bin     apps/wireless/build/esp32_gopro_canbus_wireless.bin \\
        --storage-bin apps/wireless/build/storage.bin \\
        --output      manifest.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import date, timezone
from pathlib import Path


SEMVER_RE = re.compile(
    r"^v?(?P<core>\d+\.\d+\.\d+)(?:-(?P<pre>(?:rc|beta|dev)(?:\.[0-9A-Za-z-]+)?))?$"
)


def parse_version(raw: str, channel_override: str | None = None) -> tuple[str, str]:
    """Return (version-string, channel). Raises SystemExit on bad input."""
    if channel_override == "dev":
        # Dev releases are free-form (e.g. dev-abcd123-202605281530). Strip a
        # leading 'v' if the caller passed one so the stamp displays cleanly.
        return raw.lstrip("v"), "dev"
    m = SEMVER_RE.match(raw)
    if not m:
        sys.exit(f"error: version {raw!r} does not look like vMAJOR.MINOR.PATCH[-rc.N|-beta.N]")
    core = m.group("core")
    pre = m.group("pre")
    if pre is None:
        return core, "stable"
    kind = pre.split(".", 1)[0]
    if kind == "dev":
        sys.exit("error: pass --channel dev explicitly for dev releases")
    if kind in ("rc", "beta"):
        return f"{core}-{pre}", "beta"
    sys.exit(f"error: unrecognized prerelease tag {pre!r}")


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--version", required=True,
                   help="Release tag, e.g. v0.1.0 or v0.1.0-rc.1")
    p.add_argument("--channel", choices=["dev"], default=None,
                   help="Force a channel (only 'dev' supported — stable/beta are derived from --version)")
    p.add_argument("--product", default="wireless",
                   help="Product variant slug (e.g. 'wireless'). Embedded in "
                        "the manifest as `product` and used by the browser to "
                        "compose the variant-aware OTA route. Today's only "
                        "variant is 'wireless'; future siblings drop in.")
    p.add_argument("--app-bin", required=True, type=Path,
                   help="Path to the wireless app binary (typically esp32_gopro_canbus_wireless.bin)")
    p.add_argument("--storage-bin", required=True, type=Path,
                   help="Path to the LittleFS storage image (storage.bin)")
    p.add_argument("--min-recovery-version", default="0.1.0",
                   help="Floor recovery version this build is compatible with (default: 0.1.0)")
    p.add_argument("--release-notes-url", default=None,
                   help="Optional URL for release notes shown in the web UI")
    p.add_argument("--output", required=True, type=Path,
                   help="Where to write manifest.json")
    args = p.parse_args()

    version, channel = parse_version(args.version, args.channel)

    for label, path in (("app", args.app_bin), ("storage", args.storage_bin)):
        if not path.exists():
            sys.exit(f"error: {label} binary not found at {path}")

    manifest = {
        "channel": channel,
        "product": args.product,
        "released": date.today().isoformat(),
        "app": {
            "version": version,
            "size":    args.app_bin.stat().st_size,
            "sha256":  sha256_of(args.app_bin),
            "url":     "app.bin",
        },
        "ui": {
            # App + UI ship as a pair; until the web UI carries its own version
            # (Phase 5 will populate /www/manifest.json), we tie it to the app.
            "version": version,
            "size":    args.storage_bin.stat().st_size,
            "sha256":  sha256_of(args.storage_bin),
            "url":     "storage.bin",
        },
        "min_recovery_version": args.min_recovery_version,
    }
    if args.release_notes_url:
        manifest["release_notes_url"] = args.release_notes_url

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(f"wrote {args.output}")
    print(f"  channel: {channel}")
    print(f"  product: {args.product}")
    print(f"  version: {version}")
    print(f"  app:     {manifest['app']['size']} bytes, sha={manifest['app']['sha256'][:12]}...")
    print(f"  ui:      {manifest['ui']['size']} bytes, sha={manifest['ui']['sha256'][:12]}...")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
