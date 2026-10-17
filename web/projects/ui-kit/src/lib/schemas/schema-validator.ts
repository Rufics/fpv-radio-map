import Ajv, { ErrorObject, ValidateFunction } from 'ajv';
import addFormats from 'ajv-formats';
import { LibraryKind, LIBRARY_KINDS } from '../models/library.models';
import { SCHEMAS } from './schemas.generated';

export interface SchemaIssue {
  /** JSON pointer of the offending value, '' for the root */
  path: string;
  message: string;
}

export interface SchemaResult {
  ok: boolean;
  issues: SchemaIssue[];
}

/**
 * Validates missions and library entries against the contract's JSON Schemas (draft-07),
 * with the default ajv class in strict mode plus ajv-formats — the same way the server does.
 */
export class SchemaValidator {
  private readonly ajv: Ajv;
  private readonly compiled = new Map<string, ValidateFunction>();

  constructor() {
    this.ajv = new Ajv({ allErrors: true, strict: true });
    addFormats(this.ajv);
    // common first, so the others can $ref it
    this.ajv.addSchema(SCHEMAS['library/common']);
    for (const [key, schema] of Object.entries(SCHEMAS)) {
      if (key !== 'library/common') this.ajv.addSchema(schema);
    }
  }

  private fn(key: string): ValidateFunction {
    let f = this.compiled.get(key);
    if (!f) {
      const id = SCHEMAS[key]?.['$id'];
      if (typeof id !== 'string') throw new Error(`No schema "${key}"`);
      f = this.ajv.getSchema(id);
      if (!f) throw new Error(`Schema "${id}" did not compile`);
      this.compiled.set(key, f);
    }
    return f;
  }

  validateMission(value: unknown): SchemaResult {
    return this.run('mission', value);
  }

  validateLibrary(kind: LibraryKind, value: unknown): SchemaResult {
    const file = LIBRARY_KINDS.find((k) => k.kind === kind)?.schemaFile.replace('.schema.json', '');
    if (!file) throw new Error(`Unknown library kind ${kind}`);
    return this.run(`library/${file}`, value);
  }

  private run(key: string, value: unknown): SchemaResult {
    const f = this.fn(key);
    const ok = f(value) as boolean;
    return { ok, issues: ok ? [] : (f.errors ?? []).map(SchemaValidator.describe) };
  }

  static describe(e: ErrorObject): SchemaIssue {
    let message = e.message ?? 'invalid';
    if (e.keyword === 'additionalProperties') message = `unknown field “${(e.params as { additionalProperty: string }).additionalProperty}”`;
    if (e.keyword === 'enum') message = `must be one of ${(e.params as { allowedValues: unknown[] }).allowedValues.join(', ')}`;
    if (e.keyword === 'required') message = `missing required field “${(e.params as { missingProperty: string }).missingProperty}”`;
    return { path: e.instancePath, message };
  }
}
