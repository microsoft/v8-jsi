// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Writes the Linux agent image recipes, .ado/image/azurelinux3-1espt-*.json.
// Their linux-bash-command entry carries .ado/image/linux-image-provision.sh
// and the libnode Dockerfiles it builds, compressed and base64-encoded,
// because ImageFactory passes that command on a command line where quotes and
// $ would be interpreted. Run it after changing any of those files, then
// rebuild both images.
//
//   node scripts/linux-image-recipe.ts

import * as fs from "node:fs";
import * as path from "node:path";
import * as zlib from "node:zlib";

const imageDir = path.resolve(import.meta.dirname, "..", ".ado", "image");
const files = ["libnode-linux-build.Dockerfile", "libnode-linux-smoke.Dockerfile", "linux-image-provision.sh"];
const recipes = ["azurelinux3-1espt-x64.json", "azurelinux3-1espt-arm64.json"];
const marker = "V8JSI_EOF";

// A script that writes the files, with LF line endings as on a Linux checkout
// (the Dockerfile hashes must match), and runs the provisioning script.
const lines = ["set -eu", "dir=/tmp/v8jsi-image", 'rm -rf "$dir"', 'mkdir -p "$dir"'];
for (const file of files) {
  const text = fs.readFileSync(path.join(imageDir, file), "utf-8").replaceAll("\r\n", "\n");
  if (!text.endsWith("\n") || text.split("\n").includes(marker)) {
    throw new Error(`${file} must end with a line break and have no line "${marker}"`);
  }
  lines.push(`cat > "$dir/${file}" <<'${marker}'`, text + marker);
}
lines.push('bash "$dir/linux-image-provision.sh"', "");
// Compressed to keep the command short. The gzip header is fixed (no time
// stamp, Unix as the OS), so every machine writes the same recipe.
const gzip = zlib.gzipSync(Buffer.from(lines.join("\n"), "utf-8"), { level: 9 });
gzip.writeUInt32LE(0, 4);
gzip[9] = 3;
const command = `echo ${gzip.toString("base64")} | base64 -d | gzip -d > /tmp/v8jsi-image.sh && bash /tmp/v8jsi-image.sh`;

const recipe = { artifacts: [{ name: "linux-bash-command", parameters: { command } }] };
for (const name of recipes) {
  fs.writeFileSync(path.join(imageDir, name), JSON.stringify(recipe, null, 4) + "\n");
  console.log(`Wrote ${name} (command: ${command.length} characters)`);
}
