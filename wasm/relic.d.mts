export type Value =
    | { type: "integer"; value: bigint }
    | { type: "symbol"; value: string }
    | { type: "string"; value: Uint8Array };

export interface Diagnostic {
    level: "note" | "warning" | "error";
    message: string;
    location: { file: string | null; line: number; column: number } | null;
}

export interface Evaluation {
    version: string;
    queries: { rows: Value[][] }[];
    diagnostics: Diagnostic[];
}

export interface Options {
    signal?: AbortSignal;
    timeoutMs?: number;
    maxInputBytes?: number;
    maxMemoryBytes?: number;
    maxTuples?: number;
    maxMagic?: number;
    maxRows?: number;
    maxOutputBytes?: number;
    maxDiagnosticBytes?: number;
}

export type ErrorKind = "argument" | "rejected" | "evaluation" | "limit"
    | "memory" | "internal" | "cancelled" | "timeout" | "worker";

export class RelicError extends Error {
    constructor(kind: ErrorKind, message: string, diagnostics?: Diagnostic[], engine_class?: number | null);
    readonly kind: ErrorKind;
    readonly engineClass: number | null;
    readonly diagnostics: Diagnostic[];
}

export const limits: Readonly<Required<Omit<Options, "signal">>>;
export function evaluate(program: string, options?: Options): Promise<Evaluation>;
