// firmware-proxy — Cloudflare Worker that fronts GitHub Releases with CORS,
// and serves the ESP Launchpad configuration TOML.
//
// The ESP32 device makes no outbound TCP; the user's browser fetches
// firmware blobs directly from this worker, which transparently proxies
// to github.com and adds the Access-Control-Allow-Origin header that
// raw GitHub downloads do not include.
//
// /launchpad.toml is served inline (not proxied) so ESP Launchpad has a
// stable CORS-enabled URL to load the firmware catalog from. The catalog
// lists one entry per supported product variant; new variants drop in
// alongside `wireless` by appending to SUPPORTED_VARIANTS below.
//
// Friendly per-variant manifest URLs are accepted as well:
//   /<variant>/latest-<channel>/manifest.json
// rewrites to
//   <GITHUB_REPO_PATH>/releases/download/latest-<channel>-<variant>/manifest.json
// so device firmware can speak either form (current firmware composes the
// suffixed form directly; the friendly form is for launchpad and humans).
//
// See docs/design/ota.md §2 (architecture) and §5 (manifest routing),
// and docs/releases.md for the fresh-board flash flow.

const GITHUB_REPO_PATH = "/imdacrocker/esp32_gopro_controller";

// Catalog of product variants advertised through Launchpad. Keep names
// short ASCII — they end up in URLs. Order is the order the dropdown
// shows in Launchpad. Add new variants here when they ship.
const SUPPORTED_VARIANTS = [
  {
    slug:  "wireless",
    label: "GoPro CAN-Bus Controller (Wireless)",
    readme: `Fresh-board provisioning for the wireless ESP32 GoPro CAN-Bus Controller.

After flashing, power-cycle the board. It will come up as WiFi SoftAP
\`HERO-RC-XXXXXX\` (open, no password). Join it and open http://10.71.79.1/
to add cameras and adjust settings.`,
  },
];

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
  "Access-Control-Allow-Headers": "*",
  "Access-Control-Max-Age": "86400",
};

// Keep in sync with tools/firmware-proxy/launchpad.toml. The Worker is the
// canonical source so firmware_images_url can be rewritten at request time
// from the Worker's own origin — same code works on any deployment URL.
// One [app] section per variant, each pointing at its own latest-stable
// floating release.
function launchpadToml(origin) {
  const supportedAppsList = SUPPORTED_VARIANTS.map(v => `"${v.label}"`).join(", ");
  const sections = SUPPORTED_VARIANTS.map(v => {
    const url = `${origin}${GITHUB_REPO_PATH}/releases/download/latest-stable-${v.slug}/`;
    return `[${v.label}]
chipsets = ["ESP32-S3"]
image.esp32s3 = "factory.bin"
firmware_images_url = "${url}"
readme.text = """
${v.readme}
"""`;
  }).join("\n\n");

  return `esp_toml_version = 1.0
supported_apps = [${supportedAppsList}]

${sections}
`;
}

// Friendly per-variant route:
//   /<variant>/latest-<channel>/<asset>
//     → <GITHUB_REPO_PATH>/releases/download/latest-<channel>-<variant>/<asset>
// Returns the rewritten github.com URL, or null if the path doesn't match.
function rewriteVariantRoute(pathname) {
  const m = pathname.match(/^\/([a-z0-9_-]+)\/latest-(stable|beta|dev)\/(.+)$/i);
  if (!m) return null;
  const [, variant, channel, asset] = m;
  // Only rewrite for variants the worker knows about. Unknown variants
  // fall through to the github.com pass-through (which will 404), keeping
  // the proxy honest about what it claims to support.
  if (!SUPPORTED_VARIANTS.some(v => v.slug === variant)) return null;
  return `https://github.com${GITHUB_REPO_PATH}/releases/download/latest-${channel}-${variant}/${asset}`;
}

export default {
  async fetch(request) {
    const url = new URL(request.url);

    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }

    if (url.pathname === "/launchpad.toml") {
      return new Response(launchpadToml(url.origin), {
        status: 200,
        headers: {
          ...CORS_HEADERS,
          "Content-Type": "application/toml; charset=utf-8",
          "Cache-Control": "public, max-age=300",
        },
      });
    }

    // Friendly /<variant>/latest-<channel>/... route, if matched. Falls
    // through to the github.com pass-through below for any other path
    // (including the device firmware's direct
    // <repo>/releases/download/latest-<channel>-<variant>/... requests).
    const target =
      rewriteVariantRoute(url.pathname) ??
      ("https://github.com" + url.pathname + url.search);

    const upstream = await fetch(target, {
      method: request.method,
      redirect: "follow",
    });

    const headers = new Headers(upstream.headers);
    headers.set("Access-Control-Allow-Origin", "*");
    headers.set("Access-Control-Expose-Headers",
                "Content-Length, Content-Type, ETag, Last-Modified");

    return new Response(upstream.body, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers,
    });
  },
};
