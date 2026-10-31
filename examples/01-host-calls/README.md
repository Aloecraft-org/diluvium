# Host calls in plain dlua

`host.fs`, `host.sql`, `host.exec`, `host.crypto` and `host.time` work in
the `diluvium` binary itself. It answers them directly, with the rights a
Lua script has, so there's no `--task`, no config and no DRT. Under DRT
the same file runs with only what its config grants.

## Run it

```
cd examples/01-host-calls
diluvium app.dlua
```

```
fs     a note, written through host.fs
sql    a row, through host.sql
exec   a child process, through host.exec
crypto ba7816bf8f01cfea...
time   a wall clock, in ms
```

The same program under DRT prints the same five lines, after one line on
stderr that says exec leaves the sandbox:

```
drt run --config drt.json
```

## What plain dlua answers, and where

Files and paths are the program's own, relative to where it runs. The
signing calls (`hmac`, `jwt_sign`, `jwt_verify`) use the key in
`DILUVIUM_CRYPTO_KEY` and derive their subkeys the way DRT does. That
means a token signed here verifies under a DRT that has the same key.
`crypto.turn_credential`, named keys and `host.spawn` need a deployment,
so they're refused at once with a reason. `--queue-host` turns this off
for a program that answers its own `host/calls` queue.
