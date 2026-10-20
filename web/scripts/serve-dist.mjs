// Tiny static server with SPA fallback, for the built app (used by screenshots.mjs).
// Usage: node scripts/serve-dist.mjs [build=fpv-app-mock/browser] [port=4300]
// It serves only from web/dist: the build is a folder inside it, and requests stay inside the build.
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.webmanifest': 'application/manifest+json', '.ico': 'image/x-icon', '.png': 'image/png', '.svg': 'image/svg+xml', '.woff2': 'font/woff2', '.woff': 'font/woff', '.pbf': 'application/x-protobuf' };
const DIST = resolve(fileURLToPath(new URL('../dist/', import.meta.url)));

/** The file under `root` for a request path; index.html for anything outside root or missing. */
async function fileFor(root, requested, index) {
  const file = resolve(root, requested);
  if (!file.startsWith(root + sep)) return index;
  const info = await stat(file).catch(() => null);
  return info?.isFile() ? file : index;
}

export function serve(build, port) {
  const root = resolve(DIST, build);
  if (!root.startsWith(DIST + sep)) throw new Error(`${build} is not a build inside ${DIST}`);
  const index = join(root, 'index.html');
  const server = createServer(async (req, res) => {
    // The path part of the request target, without query or fragment.
    const target = (req.url ?? '/').split(/[?#]/)[0];
    const requested = decodeURIComponent(target).replace(/^\/+/, '');
    const file = await fileFor(root, requested, index);
    try {
      const body = await readFile(file);
      res.writeHead(200, { 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream' });
      res.end(body);
    } catch {
      res.writeHead(404).end();
    }
  });
  return new Promise((ok) => server.listen(port, '127.0.0.1', () => ok(server)));
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const build = process.argv[2] ?? 'fpv-app-mock/browser';
  const port = Number(process.argv[3] ?? 4300);
  await serve(build, port);
  console.log(`Serving dist/${build} on http://127.0.0.1:${port}`);
}
