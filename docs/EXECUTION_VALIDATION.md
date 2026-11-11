# Execution validation

There is no execution to validate. No order was sent to Hyperliquid or Lighter. The account, the key, and the signer are absent on purpose.

What exists instead:

- A state machine that refuses to treat send as accept, with tests for partial fills, duplicate acks, disconnect, and a gapped fill stream.
- A queue model with tests for the cases in QUEUE_MODEL.md.
- Metric definitions with a sign convention and one synthetic numeric test.

What a validation report would have to contain, and does not, because the sample size is zero:

- predicted versus observed fill rate, with n and an interval
- time-to-fill distributions
- partial-fill counts
- slippage and adverse selection at 100ms, 500ms, 1s, 5s, 30s, using one clock domain at a time
- separate tables per venue and instrument

Until those rows exist, the execution model is implemented and tested against fixtures, and it is not validated.
