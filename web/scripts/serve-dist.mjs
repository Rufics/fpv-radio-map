// Tiny static server with SPA fallback, for the built app (used by screenshots.mjs).
// Usage: node scripts/serve-dist.mjs [dir=dist/fpv-app-mock/browser] [port=4300]
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.ico': 'image/x-icon', '.png': 'image/png', '.svg': 'image/svg+xml', '.woff2': 'font/woff2', '.woff': 'font/woff', '.pbf': 'application/x-protobuf' };

export function serve(dir, port) {
  const root = resolve(dir);
  const server = createServer(async (req, res) => {
    const path = normalize(decodeURIComponent(new URL(req.url ?? '/', 'http://x').pathname)).replace(/^(\.\.[/\\])+/, '');
    let file = join(root, path);
    try {
      if (!file.startsWith(root) || !(await stat(file)).isFile()) throw new Error();
    } catch {
      file = join(root, 'index.html');
    }
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
  const dir = process.argv[2] ?? 'dist/fpv-app-mock/browser';
  const port = Number(process.argv[3] ?? 4300);
  await serve(dir, port);
  console.log(`Serving ${dir} on http://127.0.0.1:${port}`);
}
