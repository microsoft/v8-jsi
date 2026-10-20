/** A user-facing failure: bad input, bad configuration, or a tool error. */
export class StelaError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "StelaError";
  }
}
