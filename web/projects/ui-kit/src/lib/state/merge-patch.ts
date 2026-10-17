/** JSON Merge Patch (RFC 7386). Returns a new value; the target is not modified. */
export function applyMergePatch<T>(target: T, patch: unknown): T {
  return merge(target, patch) as T;
}

function isObject(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

function merge(target: unknown, patch: unknown): unknown {
  if (!isObject(patch)) return structuredClone(patch);
  const result: Record<string, unknown> = isObject(target) ? { ...target } : {};
  for (const [key, value] of Object.entries(patch)) {
    if (value === null) delete result[key];
    else result[key] = merge(result[key], value);
  }
  return result;
}
