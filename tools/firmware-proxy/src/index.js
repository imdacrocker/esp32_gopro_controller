// firmware-proxy — Cloudflare Worker that fronts GitHub Releases with CORS,
// and serves the ESP Launchpad configuration TOML.
//
// The ESP32 device makes no outbound TCP; the user's browser fetches
// firmware blobs directly from this worker, which transparently proxies
// to github.com and adds the Access-Control-Allow-Origin header that
// raw GitHub downloads do not include.
//
// /launchpad.toml is served inline (not proxied) so ESP Launchpad has a
// stable CORS-enabled URL to load the firmware catalog from.
//
// See docs/design/ota.md §2 (architecture) and §5 (manifest routing),
// and docs/releases.md for the fresh-board flash flow.

const GITHUB_REPO_PATH = "/imdacrocker/esp32_gopro_controller";

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
  "Access-Control-Allow-Headers": "*",
  "Access-Control-Max-Age": "86400",
};

// Keep in sync with tools/firmware-proxy/launchpad.toml. The Worker is the
// canonical source so firmware_images_url can be rewritten at request time
// from the Worker's own origin — same code works on any deployment URL.
function launchpadToml(origin) {
  return `esp_toml_version = 1.0
firmware_images_url = "${origin}${GITHUB_REPO_PATH}/releases/download/latest-stable/"
supported_apps = ["GoPro CAN-Bus Controller"]

[GoPro CAN-Bus Controller]
chipsets = ["ESP32-S3"]
image.esp32s3 = "factory.bin"
readme.text = """
Fresh-board provisioning for the ESP32 GoPro CAN-Bus Controller.

After flashing, power-cycle the board. It will come up as WiFi SoftAP
\`HERO-RC-XXXXXX\` (open, no password). Join it and open http://10.71.79.1/
to add cameras and adjust settings.
"""
`;
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

    // Pass-through to github.com, preserving the path
    const target = "https://github.com" + url.pathname + url.search;
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
