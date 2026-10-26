/** JSON Merge Patch (RFC 7386). Returns a new value; the target is not modified. */
export function applyMergePatch<T>(target: T, patch: unknown): T {
  return merge(target, patch) as T;
}

/**
 * Keys a patch may not set. A patch comes from the user (the compare panel) or from a module's
 * fix; assigning "__proto__" would replace the prototype of the merged object instead of adding
 * a field, and the other two are the way to reach a prototype from an object.
 */
const FORBIDDEN_KEYS = new Set(['__proto__', 'constructor', 'prototype']);

function isObject(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

function merge(target: unknown, patch: unknown): unknown {
  if (!isObject(patch)) return structuredClone(patch);
  const result: Record<string, unknown> = isObject(target) ? { ...target } : {};
  for (const [key, value] of Object.entries(patch)) {
    if (FORBIDDEN_KEYS.has(key)) continue;
    if (value === null) delete result[key];
    else result[key] = merge(result[key], value);
  }
  return result;
}
