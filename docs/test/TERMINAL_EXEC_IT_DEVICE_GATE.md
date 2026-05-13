# Terminal `exec -it` Device Gate

This gate protects the UI route that opens an interactive container terminal
through the Engine exec API. It is a hard gate only when a real Engine container
ID or name is required by the caller.

## Pass/fail rule

- `planned-skip is evidence, not success`.
- If `HardGateRequired` is true, `Status: planned-skip` must fail the gate.
- If a real container is required, the artifact must contain `Success: true`
  and a non-empty `Container` value.
- Quick smoke may still write a planned-skip artifact when no container exists,
  but that artifact is never counted as a hard-gate pass.

## Device artifacts

The runner collects these files into `PDOCKER_SMOKE_ARTIFACT_DIR`:

- `ui-it-selftest-latest.json`
- `engine-exec-input-latest.jsonl`

The skip artifact must include:

- `Status: "planned-skip"`
- `Success: false`
- `DeviceProofAttempted: false`
- `HardGateRequired`
- `RequiredEvidence`
- `Evidence`

## Required evidence names

The gate validates the following evidence names for a real-container run:

- `enter-single-submit`: Enter submits a command once and produces
  `pdocker-ui-it-ok`.
- `ctrl-c-interrupts-without-literal-c`: Ctrl-C interrupts `sleep` and returns
  to the shell without injecting literal `c` into the command stream.
- `arrow-up-reaches-readline-history`: Arrow-up reaches shell history/readline
  and does not print raw escape bytes as text.
- `top-starts-on-tty`: `top` can start against a controlling TTY.
- `q-quits-top`: `q` exits `top`, after which the shell accepts another
  command.
- `resize-route-is-observable`: the Engine exec resize route is observable
  through terminal diagnostics. Until the Android artifact records a dedicated
  resize-success event, the runner accepts the existing Engine exec stream
  diagnostics as the observable resize-route proof and keeps this evidence name
  explicit so the contract cannot silently disappear.

## Regression symptoms this gate is meant to catch

- Enter requires two presses or moves horizontally instead of submitting.
- JP IME Ctrl-C injects literal `c`.
- Arrow keys print escape sequences instead of reaching readline.
- `top` renders as a non-TTY stream or cannot be quit with `q`.
- A planned-skip artifact is treated as a successful required test.
