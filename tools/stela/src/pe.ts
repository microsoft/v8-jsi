// A minimal PE reader (design §12.3): the machine type, the export names, and
// one resource, read from the file's bytes. It never loads or runs the
// binary, reads any architecture on any host, and bounds-checks every read,
// since the binary is untrusted input.

import { StelaError } from "./errors.ts";

/** The target triple of each PE machine type Stela knows. */
export const MACHINE_TARGETS: Readonly<Record<number, string>> = {
  0x8664: "x86_64-pc-windows-msvc",
  0x014c: "i686-pc-windows-msvc",
  0xaa64: "aarch64-pc-windows-msvc",
};

/** The largest resource the reader returns. */
export const MAX_RESOURCE_BYTES = 16 * 1024 * 1024;
const MAX_NAME_BYTES = 4096;
const MAX_EXPORT_NAMES = 1 << 20;
const MAX_TOTAL_NAME_BYTES = 16 * 1024 * 1024;

export interface PeFile {
  readonly machine: number;
  /** Export names, in export-table order. */
  readonly exports: readonly string[];
  /** The data of resource `type`/`id` (first language), or undefined. */
  resource(type: string, id: number): Buffer | undefined;
}

interface Section {
  readonly va: number;
  readonly virtualSize: number;
  readonly rawOffset: number;
  readonly rawSize: number;
}

export function isPe(bytes: Buffer): boolean {
  return bytes.length >= 2 && bytes[0] === 0x4d && bytes[1] === 0x5a;
}

export function parsePe(bytes: Buffer, what = "binary"): PeFile {
  const fail = (message: string): never => {
    throw new StelaError(`${what}: not a valid PE file: ${message}`);
  };
  const need = (offset: number, length: number, field: string) => {
    if (!Number.isSafeInteger(offset) || offset < 0 || length < 0 || offset + length > bytes.length) {
      fail(`${field} is out of bounds`);
    }
  };
  const u16 = (offset: number, field: string) => (need(offset, 2, field), bytes.readUInt16LE(offset));
  const u32 = (offset: number, field: string) => (need(offset, 4, field), bytes.readUInt32LE(offset));

  if (!isPe(bytes)) fail("no MZ signature");
  const pe = u32(0x3c, "e_lfanew");
  if (u32(pe, "PE signature") !== 0x00004550) fail("no PE signature");
  const coff = pe + 4;
  const machine = u16(coff, "machine");
  const sectionCount = u16(coff + 2, "section count");
  const optionalSize = u16(coff + 16, "optional header size");
  const optional = coff + 20;
  need(optional, optionalSize, "optional header");
  const magic = u16(optional, "optional header magic");
  let directoriesAt: number;
  if (magic === 0x10b) directoriesAt = 96;
  else if (magic === 0x20b) directoriesAt = 112;
  else return fail(`unknown optional header magic 0x${magic.toString(16)}`);
  if (optionalSize < directoriesAt) fail("optional header is too small");
  const directoryCount = Math.min(u32(optional + directoriesAt - 4, "directory count"), (optionalSize - directoriesAt) >>> 3);
  const directory = (index: number) =>
    index < directoryCount
      ? {
          rva: u32(optional + directoriesAt + index * 8, "data directory"),
          size: u32(optional + directoriesAt + index * 8 + 4, "data directory"),
        }
      : { rva: 0, size: 0 };

  const sections: Section[] = [];
  const sectionTable = optional + optionalSize;
  for (let i = 0; i < sectionCount; i++) {
    const at = sectionTable + i * 40;
    need(at, 40, "section header");
    sections.push({
      virtualSize: bytes.readUInt32LE(at + 8),
      va: bytes.readUInt32LE(at + 12),
      rawSize: bytes.readUInt32LE(at + 16),
      rawOffset: bytes.readUInt32LE(at + 20),
    });
  }

  /** The file offset of `length` bytes at `rva`, which must lie in one section's raw data. */
  const offsetOf = (rva: number, length: number, field: string): number => {
    for (const s of sections) {
      const span = Math.max(s.virtualSize, s.rawSize);
      if (rva < s.va || rva - s.va >= span) continue;
      if (rva - s.va + length > s.rawSize) fail(`${field} runs past its section's data`);
      const offset = s.rawOffset + (rva - s.va);
      need(offset, length, field);
      return offset;
    }
    return fail(`${field} at RVA 0x${rva.toString(16)} is in no section`);
  };

  // Names may share bytes (every pointer can name the same string), so the
  // decoded total is capped as well as each name.
  let nameBytes = 0;
  const cString = (rva: number, field: string): string => {
    const start = offsetOf(rva, 1, field);
    const end = bytes.subarray(start, start + MAX_NAME_BYTES + 1).indexOf(0);
    if (end < 0) fail(`${field} isn't terminated`);
    offsetOf(rva, end + 1, field);
    nameBytes += end;
    if (nameBytes > MAX_TOTAL_NAME_BYTES) fail(`the export names exceed ${MAX_TOTAL_NAME_BYTES} bytes`);
    return bytes.toString("latin1", start, start + end);
  };

  const exports: string[] = [];
  const exportDir = directory(0);
  if (exportDir.rva !== 0 && exportDir.size !== 0) {
    const dir = offsetOf(exportDir.rva, 40, "export directory");
    const count = bytes.readUInt32LE(dir + 24);
    const namesRva = bytes.readUInt32LE(dir + 32);
    if (count > MAX_EXPORT_NAMES) fail("too many export names");
    if (count > 0) {
      const names = offsetOf(namesRva, count * 4, "export name table");
      for (let i = 0; i < count; i++) exports.push(cString(bytes.readUInt32LE(names + i * 4), "export name"));
    }
  }

  const resourceDir = directory(2);
  const resource = (type: string, id: number): Buffer | undefined => {
    if (resourceDir.rva === 0 || resourceDir.size === 0) return undefined;
    const base = resourceDir.rva;
    const entries = (relative: number, field: string) => {
      const at = offsetOf(base + relative, 16, field);
      const count = bytes.readUInt16LE(at + 12) + bytes.readUInt16LE(at + 14);
      const table = offsetOf(base + relative + 16, count * 8, field);
      return Array.from({ length: count }, (_, i) => ({
        name: bytes.readUInt32LE(table + i * 8),
        data: bytes.readUInt32LE(table + i * 8 + 4),
      }));
    };
    const nameOf = (field: number): string | number => {
      if ((field & 0x80000000) === 0) return field & 0xffff;
      const relative = field & 0x7fffffff;
      const at = offsetOf(base + relative, 2, "resource name");
      const length = bytes.readUInt16LE(at);
      const text = offsetOf(base + relative + 2, length * 2, "resource name");
      return bytes.toString("utf16le", text, text + length * 2);
    };
    const isDirectory = (data: number) => (data & 0x80000000) !== 0;

    const typeEntry = entries(0, "resource directory").find((e) => {
      const name = isDirectory(e.data) ? nameOf(e.name) : undefined;
      return typeof name === "string" && name.toUpperCase() === type.toUpperCase();
    });
    if (typeEntry === undefined) return undefined;
    const idEntry = entries(typeEntry.data & 0x7fffffff, "resource directory").find(
      (e) => isDirectory(e.data) && nameOf(e.name) === id,
    );
    if (idEntry === undefined) return undefined;
    const language = entries(idEntry.data & 0x7fffffff, "resource directory").find((e) => !isDirectory(e.data));
    if (language === undefined) return undefined;
    const entry = offsetOf(base + language.data, 16, "resource data entry");
    const dataRva = bytes.readUInt32LE(entry);
    const size = bytes.readUInt32LE(entry + 4);
    if (size > MAX_RESOURCE_BYTES) fail(`resource ${type} ${id} is larger than ${MAX_RESOURCE_BYTES} bytes`);
    const offset = offsetOf(dataRva, size, "resource data");
    return Buffer.from(bytes.subarray(offset, offset + size));
  };

  return { machine, exports, resource };
}
