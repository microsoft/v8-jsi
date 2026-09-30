# Sandbox guest host

`v8host.exe` loads the selected JSI engine, completes sandbox lockdown, and runs
JavaScript. The broker owns the policy and closes the message channel when the
run is finished.

## Results

- With no nonempty `V8HOST_GUEST_JS`, the host runs its built-in demo. Success
  requires the demo file probes and both string and binary message exchanges.
  The existing x86 exception for unsupported file brokering is unchanged.
- With `V8HOST_GUEST_JS`, the host reads that file before lockdown and runs it
  as the custom guest. An empty file is valid. A requested file that cannot be
  read is an input error, not permission to fall back to the demo.
- Custom guests do not need demo files or any particular number or type of
  messages. Exit `0` means the required sandbox checks passed, guest evaluation
  completed, and the message loop ended normally when the broker closed it.
  It does **not** certify an application-specific result sent by the guest.
  The broker and guest define that protocol; the host does not interpret it.
- Exit `13` reports a demo-check, JavaScript evaluation/callback, message, or
  message-loop failure. Failed message sends throw a JavaScript error.
- Exit `23` reports a returned lockdown or security-validation failure. Fatal
  failures inside the sandbox may terminate with their own nonzero codes.
- Exit `24` reports an unreadable guest path or guest file.

All modes retain mandatory lockdown and verification of requested ACG.
Neither custom-guest selection nor result reporting disables security checks.

## Tests

After the sandbox engine has been built and staged beside the host, run from
the repository root:

```powershell
node .\scripts\sbox-build.ts --target-cpu x64 --target v8host_tests
& .\deps\chromium\out\sandbox-x64\v8host_tests.exe --all
```

Use `--case custom-strings-only` to run one case. The tests exercise the
production sandbox DLL and host with synthetic scripts, check child exit codes
and replies, and cover both guest modes and failure paths. The runner is
`testonly`; it is not included in the production build group or NuGet staging.
