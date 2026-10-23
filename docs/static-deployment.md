# Deploy a Vite or static frontend

The C server can host built frontend files alongside the print interface and
API. A mount gives each app a stable URL path, and Vite SPA mode serves the
app's `index.html` when the browser opens a client-side route.

## Build and install a Vite app

Run this from the server repository. The source directory must have a
`package-lock.json` (or `npm-shrinkwrap.json`) and a Vite `build` script.

```sh
./scripts/manage.sh static build dashboard /path/to/dashboard https://print.example.com
```

The command runs `npm ci`, builds with Vite base `/dashboard/`, copies `dist/`
into `public/apps/dashboard`, and adds this mount to `config/server.conf`:

```ini
static_mount = /dashboard|apps/dashboard|spa
```

It prints `https://print.example.com/dashboard/`. Restart the server to load a
new mount. Later `static build` runs replace the deployed files while keeping
the mount. Build output containing symbolic links is refused.

For an existing build, build Vite yourself with the correct base and install
the result:

```sh
cd /path/to/dashboard
npm ci
npm run build -- --base=/dashboard/
cd /path/to/server
./scripts/manage.sh static add dashboard /path/to/dashboard/dist https://print.example.com
```

The server's default MIME table includes JavaScript modules, CSS, JSON,
WebAssembly, SVG, common images, and web fonts. Add a mapping to
`config/mime.types` if your build uses another file extension.

## Manage deployments

```sh
./scripts/manage.sh static list
./scripts/manage.sh static build dashboard /path/to/dashboard https://print.example.com
./scripts/manage.sh static remove dashboard
```

`remove` asks before deleting the mount and its deployed files. The static
commands update `config/server.conf`; restart the server after adding or
removing a mount. A replacement keeps the same mount configuration. The script
stages the new build and swaps the app directory; stop the server during that
short swap window if you need to avoid a transient 404 while publishing.

## Make it public

Point your domain's DNS to the host and configure the public TLS proxy as
described in the [domain deployment guide](domain-setup/README.md). Its existing
catch-all site block proxies every path to this server, so no additional proxy
route is needed for a mounted app. Keep the C server bound to loopback behind
the proxy. After DNS and HTTPS are active, the printed URL is public.

Applications should use Vite's base path for bundled assets. API requests can
still use same-origin absolute paths such as `/api/me`. Treat frontend code as
public: never put admin credentials, signing secrets, payment secrets, or
private service configuration in Vite environment variables prefixed with
`VITE_` or in the generated build.
