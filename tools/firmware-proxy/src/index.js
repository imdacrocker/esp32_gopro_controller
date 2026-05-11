// firmware-proxy — Cloudflare Worker that fronts GitHub Releases with CORS.
//
// The ESP32 device makes no outbound TCP; the user's browser fetches
// firmware blobs directly from this worker, which transparently proxies
// to github.com and adds the Access-Control-Allow-Origin header that
// raw GitHub downloads do not include.
//
// See ota_design.md §2 (architecture) and §5 (manifest routing).

export default {
  async fetch(request) {
    const url = new URL(request.url);

    // CORS preflight
    if (request.method === "OPTIONS") {
      return new Response(null, {
        status: 204,
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
          "Access-Control-Allow-Headers": "*",
          "Access-Control-Max-Age": "86400",
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
