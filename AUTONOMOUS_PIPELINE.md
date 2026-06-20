# PS4 Cast Autonomous Dev Pipeline

This repo can build, deploy, launch, and smoke-test PS4 Cast without the
DirectPackageInstaller GUI. The PS4 must be jailbroken with GoldHEN running, and
GoldHEN's payload server must be reachable on port 9090.

## Defaults

- PS4 IP: `192.168.1.253`
- Mac host IP: `192.168.1.139`
- App HTTP UI/status: `http://192.168.1.253:8080`
- GoldHEN payload server: `http://192.168.1.253:9090/payload`

Override with environment variables when needed:

```sh
export PS4_IP=192.168.1.253
export HOST_IP=192.168.1.139
```

## One-command cycle

Use this for normal iteration:

```sh
PS4_IP=192.168.1.253 HOST_IP=192.168.1.139 scripts/dev-cycle.sh
```

The cycle does this:

1. Builds the current version from `app/Makefile`.
2. Copies the package to `dist/PS4-Cast-vXX.XX.pkg`.
3. Closes PS4 Cast if it is running.
4. Deletes the old installed PS4 Cast package by default.
5. Pushes the new package using GoldHEN + the embedded DPI payload.
6. Launches PS4 Cast with the app-control payload.
7. Polls `/status` until the expected version responds.
8. Runs the built-in smoke requests from `scripts/test-ps4cast.sh`.

To reinstall without deleting first:

```sh
PS4CAST_DELETE_BEFORE_INSTALL=0 scripts/dev-cycle.sh
```

## Manual stages

Build only:

```sh
./build.sh
```

Close the running app:

```sh
scripts/close-ps4cast.sh
```

Delete installed PS4 Cast:

```sh
scripts/delete-ps4cast.sh
```

Install the current `dist/PS4-Cast-vXX.XX.pkg`:

```sh
python3 scripts/push-goldhen-dpi.py --host "$HOST_IP" --ps4 "$PS4_IP" --keepalive 180
```

Open PS4 Cast:

```sh
scripts/open-ps4cast.sh "$(awk -F':= *' '/^VERSION/{gsub(/[ \t]/,\"\",$2);print $2}' app/Makefile)"
```

Read live status:

```sh
curl -sS "http://$PS4_IP:8080/status"
```

Run smoke tests:

```sh
scripts/test-ps4cast.sh
```

Run a real local MP4 playback test:

```sh
HOST_IP=192.168.1.139 PS4_IP=192.168.1.253 scripts/test-local-video.sh test.mp4
```

## Notes For Agents

- `scripts/push-goldhen-dpi.py` serves both the BGFT manifest and the PKG file.
  The package server supports `HEAD` and HTTP byte `Range` requests because BGFT
  may fetch the file in segments. Keep it alive for the full `--keepalive`
  window; do not stop it right after the first package request, because BGFT can
  make follow-up range requests while installing.
- If install says the manifest was fetched but the app is not installed, check
  package-server logs for `GET /PS4-Cast-...pkg` and range requests.
- If launch succeeds, `/status` must report the same `ver` as `app/Makefile`.
- The `/status` JSON includes playback `debug` and `pad` diagnostics. Use `pad`
  while pressing TV remote buttons to see whether HDMI-CEC input reaches
  homebrew through `scePad`.
- The app is signed with the conservative fake-self profile in `app/Makefile`.
  Do not reintroduce privileged auth-info unless testing a dedicated escalation
  branch; it previously caused system software crashes on this console.
