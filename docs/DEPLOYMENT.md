# Deployment

Paper only.

## Process

On a Linux host with `g++` and Python 3.10+:

```bash
make test
make asan
python3 python/record_public.py --seconds 30 --out data/live/$(date -u +%Y%m%dT%H%M%SZ)
```

Run the recorder under `systemd` or `supervisord` if you want it to restart. A unit is not shipped because this environment is not that host, and a unit that pointed at a key file would be the wrong default. Restarting the recorder does not require local order state: it has no orders. Restarting a future execution process does. On startup it must query the venue open orders and fills, and any local order still `PendingSubmit` or `Live` that the venue does not confirm becomes `Unknown` until that query returns. The journal is a belief, not a position.

## Health

The recorder writes `capture_summary.json` with message counts and nonce gaps. A gap above zero on Lighter means the book must be resubscribed; the C++ book marks `Gap` and `RiskEngine` then returns `BadBook`. Hyperliquid has no sequence, so a stall is a feed-age failure, not a gap counter.

Disk: the journal rotates at the writer's byte limit (default 64 MiB). The recorder does not delete old files. Watch the volume yourself. There is no alert sender in this tree.

Logs: the Python tools print JSON to stdout. That is the log. The C++ tests print `spark tests passed` or a failing expression.

Shutdown: the recorder closes the socket in a `finally`. There is no cancel-all because there is no order. If a future process has live orders, shutdown must set the kill switch first, then send the venue cancel-all, then wait for the cancel acknowledgements, and if the session is already dead it must reconnect and reconcile before it exits `Unknown`. Do not exit on the assumption that the cancels landed.

## Secrets

Do not put a private key in `config/`. The paper file has none. Environment variables for a future signer should be injected by the host, not committed. This repo's `.gitignore` does not yet name a secrets file because there is no secrets file.

## This preview

The browser desk is a paper view of the public books plus the same quote math. It is not the deployment of the C++ process. Killing quotes in the desk does not touch an exchange.
