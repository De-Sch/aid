#!/usr/bin/env bash
# Atomic redeploy of the AID2.0 daemon + both plugin .so files.
#
# WHY: the daemon runs from build/src/aid but dlopen's its plugins from
# $AID_DEPLOY_ROOT/plugins/*.so. If a value-type that crosses the plugin
# boundary changes and only one side is rebuilt, their layouts disagree and the
# process corrupts its heap -> SIGSEGV on the dashboard or on SIGTERM (the
# "sigterm crash"). The PluginAbiTag guard now turns that into a clean startup
# REFUSAL, but the real cure is to never let them drift: this script rebuilds
# and ships the daemon binary and BOTH plugins together, as one step.
#
# Usage: ./scripts/deploy.sh [--no-restart]
# Env:   AID_DEPLOY_ROOT  (default: ~/aid-dev)  -- mirrors the prod filesystem;
#                          plugins go in $AID_DEPLOY_ROOT/plugins, config in
#                          $AID_DEPLOY_ROOT/etc/config.json.
set -euo pipefail

cd "$(dirname "$0")/.."

# Export so the daemon child inherits it: config.json paths reference
# ${AID_DEPLOY_ROOT} (see feat(config) 672cc81 path expansion), and the daemon
# refuses to start if it is unset. deploy.sh is the launcher, so it must supply
# the env the config assumes.
export AID_DEPLOY_ROOT="${AID_DEPLOY_ROOT:-$HOME/aid-dev}"
DEPLOY_ROOT="$AID_DEPLOY_ROOT"
PLUGIN_DIR="$DEPLOY_ROOT/plugins"
CONFIG="$DEPLOY_ROOT/etc/config.json"
DAEMON_BIN="build/src/aid"
RESTART=1

for arg in "$@"; do
    case "$arg" in
        --no-restart) RESTART=0 ;;
        *) echo "deploy.sh: unknown arg '$arg'" >&2; exit 2 ;;
    esac
done

# BF3 stale-plugin guard. The daemon and both plugins embed the same greppable
# behaviour-contract token (aid::abi::kPluginContractTag, e.g.
# "AID_PLUGIN_CONTRACT=2") in their .rodata. extract_contract_tag pulls it back
# out of any binary. The PluginAbiTag guard only catches value-type LAYOUT
# drift; it does NOT catch a same-layout `.so` that simply lags the daemon — the
# trap where a pre-callHandler-drop OpenProject `.so` loaded cleanly yet emitted
# nothing new. Comparing each plugin's token against the freshly-built daemon's
# turns that into a loud deploy failure instead of silent degraded behaviour.
extract_contract_tag() {
    # $1 = path to an ELF binary (daemon or plugin .so). Echoes the token, or
    # nothing if absent. `strings -a` scans the whole file (incl. .rodata).
    # The trailing `|| true` is load-bearing: with `set -euo pipefail`, grep
    # exiting 1 on NO MATCH (exactly the stale-/legacy-.so case) would otherwise
    # trip `set -e` at the caller's `got=$(...)` assignment and kill the script
    # SILENTLY — swallowing the loud FATAL diagnostic this guard exists to emit.
    strings -a "$1" 2>/dev/null | grep -oE 'AID_PLUGIN_CONTRACT=[0-9]+' | sort -u | head -n1 || true
}

# Config-driven libPath names (see running-daemon-live notes): the daemon loads
# these exact filenames from $PLUGIN_DIR.
declare -A PLUGINS=(
    ["build/lib/adapters/openproject_plugin/aid_openproject_plugin.so"]="$PLUGIN_DIR/openproject_plugin.so"
    ["build/lib/adapters/davical_plugin/aid_davical_plugin.so"]="$PLUGIN_DIR/davical_plugin.so"
)

echo "==> 1/4 build"
./scripts/build.sh

# Verify every artifact exists before we touch the running system.
[[ -x "$DAEMON_BIN" ]] || { echo "deploy.sh: missing daemon $DAEMON_BIN" >&2; exit 1; }
for src in "${!PLUGINS[@]}"; do
    [[ -f "$src" ]] || { echo "deploy.sh: missing built plugin $src" >&2; exit 1; }
done
mkdir -p "$PLUGIN_DIR"

# BF3: behaviour-contract gate — run BEFORE we stop the running daemon so a
# stale build aborts the deploy without leaving the system down. The daemon
# binary is the source of truth; every plugin we ship must match its token.
EXPECTED_CONTRACT="$(extract_contract_tag "$DAEMON_BIN")"
if [[ -z "$EXPECTED_CONTRACT" ]]; then
    echo "deploy.sh: FATAL: daemon $DAEMON_BIN embeds no AID_PLUGIN_CONTRACT token" >&2
    echo "  (stale build, or kPluginContractTag not linked in) — rebuild from current source." >&2
    exit 1
fi
echo "==> contract gate: daemon expects $EXPECTED_CONTRACT"
for src in "${!PLUGINS[@]}"; do
    got="$(extract_contract_tag "$src")"
    if [[ -z "$got" ]]; then
        echo "deploy.sh: FATAL: built plugin $src exports no AID_PLUGIN_CONTRACT token" >&2
        echo "  -> this is a stale/legacy .so lagging the daemon; rebuild it. Refusing to deploy." >&2
        exit 1
    fi
    if [[ "$got" != "$EXPECTED_CONTRACT" ]]; then
        echo "deploy.sh: FATAL: built plugin $src contract $got != daemon $EXPECTED_CONTRACT" >&2
        echo "  -> stale plugin lagging the daemon; rebuild it. Refusing to deploy." >&2
        exit 1
    fi
    echo "    contract $got OK  $src"
done

if [[ "$RESTART" -eq 1 ]]; then
    echo "==> 2/4 stop running daemon"
    if pkill -TERM -f "$DAEMON_BIN"; then
        # Known port-bind race: a freshly-killed daemon keeps the listen port
        # bound briefly, so an immediate relaunch races. Always wait.
        echo "    sent SIGTERM; waiting 10s for clean teardown + port release"
        sleep 10
    else
        echo "    no running daemon found"
    fi
else
    echo "==> 2/4 stop running daemon (skipped: --no-restart)"
fi

echo "==> 3/4 copy plugins (write-temp-then-rename so a load never sees a half-written .so)"
for src in "${!PLUGINS[@]}"; do
    dst="${PLUGINS[$src]}"
    tmp="$dst.tmp.$$"
    cp -f "$src" "$tmp"
    chmod 0640 "$tmp"
    mv -f "$tmp" "$dst"
    # BF3 post-copy re-verify: re-extract from the file that actually landed, in
    # case the copy itself corrupted/truncated it. The pre-stop gate above
    # already cleared the common stale-build case; this guards the copy step.
    deployed="$(extract_contract_tag "$dst")"
    if [[ "$deployed" != "$EXPECTED_CONTRACT" ]]; then
        echo "deploy.sh: FATAL: just-copied $dst contract '${deployed:-<none>}' != daemon $EXPECTED_CONTRACT" >&2
        echo "  -> copy produced a stale/corrupt .so; the daemon would refuse it at startup. Aborting." >&2
        exit 1
    fi
    echo "    $(sha256sum "$dst" | cut -c1-12)  $deployed  $dst"
done

if [[ "$RESTART" -eq 1 ]]; then
    echo "==> 4/4 restart daemon"
    [[ -f "$CONFIG" ]] || { echo "deploy.sh: missing config $CONFIG" >&2; exit 1; }
    nohup "$DAEMON_BIN" "$CONFIG" >>"$DEPLOY_ROOT/daemon.out" 2>&1 &
    daemon_pid=$!
    echo "    started $DAEMON_BIN (pid $daemon_pid) with $CONFIG; logs -> $DEPLOY_ROOT/daemon.out"
    # Post-launch liveness gate. WHY: a bad config (e.g. an unset env var
    # referenced in a ${VAR} path) makes the daemon exit within a second, yet
    # this script would still print "deploy complete" and leave 8088 dead. Wait
    # a beat, then confirm the process is still alive; surface its last log lines
    # and fail loudly if it died so the deploy reflects reality.
    sleep 2
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
        echo "deploy.sh: FATAL: daemon (pid $daemon_pid) exited within 2s of launch." >&2
        echo "  --- last 20 lines of $DEPLOY_ROOT/daemon.out ---" >&2
        tail -n 20 "$DEPLOY_ROOT/daemon.out" >&2
        exit 1
    fi
    echo "    daemon still alive after 2s"
else
    echo "==> 4/4 restart daemon (skipped: --no-restart)"
    echo "    start it yourself with: $DAEMON_BIN $CONFIG"
fi

echo "==> deploy complete"
