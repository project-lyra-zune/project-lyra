# Serve the mod repository (feed.json + mods/*.zmod) from nginx. Content is baked
# in at build time by `content/` (generated with `mod-apply.py feed --out content
# --base-url https://repo.zune.moe`); the container is stateless. Rebuild to publish.
FROM nginx:1.27-alpine
COPY content /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD wget -qO- http://127.0.0.1/healthz >/dev/null 2>&1 || exit 1
