# Releases & OTA

The full system design is in [`design/ota.md`](design/ota.md). This page is the operator's guide: how the model works at a glance, and how to cut and promote a release.

## Model

- **Channels.** `stable` and `beta` published online (separate Cloudflare Worker routes). `dev` is local-only via `dev.ps1`.
- **One binary per version-variant.** Beta and stable use byte-identical binaries for the same `vX.Y.Z-<variant>` tag — promotion is a pure pointer-move on the `latest-stable-<variant>` floating tag, not a rebuild. Today's matrix is `[wireless]` (Phase 4 — see `docs/multi-variant-restructure-plan.md`); the per-variant suffix is in place so future siblings drop in.
- **App + UI ship as a pair.** Each release publishes 5 assets:
  - `manifest.json` — declares both SHA-256 hashes
  - `app.bin` — the wireless app
  - `storage.bin` — LittleFS UI image
  - `recovery.bin` — raw recovery binary; kept for niche reflash scenarios, not browser OTA
  - `factory.bin` — single-shot fresh-board image (bootloader + partition table + recovery + main + UI). Consumed by ESP Launchpad and `flash_factory.ps1`
- **Browser is the only outbound proxy.** ESP32 stays a SoftAP; the user's phone/laptop fetches signed manifests + blobs from the Cloudflare Worker and POSTs them to the device. CORS is added by the Worker (GitHub Releases doesn't ship CORS headers).

---

## Cutting a release

1. Bump the root `VERSION` file (e.g. `0.2.1` → `0.2.2`), commit, push.
2. *(only if recovery code changed)* Bump `CONFIG_APP_PROJECT_VER` in `apps/recovery/sdkconfig.defaults` independently.
3. **Actions → release-beta → Run workflow.** Runs the variant matrix; for each variant publishes `v$VERSION-<variant>` as prerelease and moves the `latest-beta-<variant>` floating tag.
4. Test on a device: channel = beta → Check for updates → Install. Validates the new bytes.
5. **Actions → release-promote → Run workflow** with `tag = v$VERSION` (the unsuffixed base — the workflow appends `-<variant>` per matrix entry). Flips each variant's source release out of prerelease and moves `latest-stable-<variant>` to the same bytes (no rebuild).
6. Bump `VERSION` to the next line (e.g. `0.2.3`) so subsequent betas don't collide with the released stable.

---

## Dev-channel version marker

Local builds on `main` carry `CONFIG_APP_PROJECT_VER="0.1.1-dev"` so it's obvious when the device is running an uncommitted/unstamped binary. CI workflows rewrite this line at build time from the tag, so `-dev` never leaks into published releases.

Every `idf.py build` also updates the `build_date`/`build_time` fields in `/api/version`, surfaced as a "Built" row in the Updates panel — that's how you confirm a flash actually took.
