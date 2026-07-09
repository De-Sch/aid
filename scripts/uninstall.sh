#!/usr/bin/env bash
# =============================================================================
# AID2.0 uninstaller — removes the installed AID daemon from a system.
#
# Removes what scripts/install.sh created: the systemd service, the daemon +
# aid-admin binaries, the state tree (auth.db, WAL, plugins, UI), the config,
# the logs, and the dedicated aiddaemon system user.
#
# KEEPS (by design): the git clone (this repo, incl. build-release/), and every
# apt package / Node that install.sh added — those are shared/expensive and not
# AID-specific. Delete the clone by hand if you want the ~1.6 GB build scratch
# back; that is independent of this script.
#
# Usage: sudo ./scripts/uninstall.sh [--yes] [--keep-data]
#   --yes        skip the confirmation prompt
#   --keep-data  remove only the service + /usr/local/bin binaries; KEEP
#                /var/lib/aid-daemon (auth.db, WAL, plugins, UI), /etc/aid-daemon
#                (config), and the logs — so a later install.sh is an UPGRADE.
# =============================================================================
set -euo pipefail

DEPLOY_ROOT="/var/lib/aid-daemon"
CONFIG_DIR="/etc/aid-daemon"
LOG_DIR="/var/log/aid-daemon"
BIN_DIR="/usr/local/bin"
UNIT="/etc/systemd/system/aid-daemon.service"

c_red=$'\033[31m'; c_grn=$'\033[32m'; c_ylw=$'\033[33m'; c_cyn=$'\033[36m'; c_bld=$'\033[1m'; c_rst=$'\033[0m'
info() { printf '%s==>%s %s\n' "$c_cyn" "$c_rst" "$*"; }
ok()   { printf '%s  ok%s %s\n' "$c_grn" "$c_rst" "$*"; }
warn() { printf '%s WARN%s %s\n' "$c_ylw" "$c_rst" "$*" >&2; }
die()  { printf '%sFATAL%s %s\n' "$c_red" "$c_rst" "$*" >&2; exit 1; }

ASSUME_YES=0
KEEP_DATA=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --yes|-y)   ASSUME_YES=1; shift ;;
        --keep-data) KEEP_DATA=1; shift ;;
        -h|--help)  grep -E '^# ' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown arg '$1' (usage: sudo ./scripts/uninstall.sh [--yes] [--keep-data])" ;;
    esac
done

[[ "$(id -u)" -eq 0 ]] || die "must run as root: sudo ./scripts/uninstall.sh"

# Which user does the service run as? Read it from the unit BEFORE we remove it,
# so we only ever delete the dedicated 'aiddaemon' account — never a --run-as
# login user the operator chose.
SERVICE_USER="$(sed -nE 's/^User=(.+)$/\1/p' "$UNIT" 2>/dev/null | head -n1 || true)"
SERVICE_USER="${SERVICE_USER:-aiddaemon}"

# Nothing installed?
if [[ ! -f "$UNIT" && ! -e "$BIN_DIR/aid" && ! -d "$DEPLOY_ROOT" && ! -d "$CONFIG_DIR" ]]; then
    ok "no AID installation found — nothing to do."
    exit 0
fi

if [[ "$ASSUME_YES" -ne 1 ]]; then
    printf '%sThis will remove the AID service and binaries.%s\n' "$c_bld" "$c_rst"
    if [[ "$KEEP_DATA" -eq 1 ]]; then
        printf '  --keep-data: KEEPING %s, %s, and logs (reinstall = upgrade).\n' "$DEPLOY_ROOT" "$CONFIG_DIR"
    else
        printf '  %sAlso DELETES all state: %s (auth.db, WAL, plugins, UI), %s (config + secrets), and %s.%s\n' \
            "$c_ylw" "$DEPLOY_ROOT" "$CONFIG_DIR" "$LOG_DIR" "$c_rst"
        printf '  The dedicated user %s will be removed.\n' "$SERVICE_USER"
    fi
    printf '  The git clone and all apt/Node packages are KEPT.\n'
    read -r -p "  Type 'yes' to proceed: " ans
    [[ "$ans" == yes || "$ans" == y ]] || die "aborted."
fi

# 1. Stop + disable + remove the service.
if [[ -f "$UNIT" ]]; then
    info "stopping and disabling the service"
    systemctl disable --now aid-daemon 2>/dev/null || true
    rm -f "$UNIT"
    systemctl daemon-reload
    systemctl reset-failed aid-daemon 2>/dev/null || true
    ok "service removed"
else
    warn "no unit at $UNIT (already gone)"
fi

# 2. Remove the binaries.
rm -f "$BIN_DIR/aid" "$BIN_DIR/aid-admin"
ok "removed $BIN_DIR/aid, $BIN_DIR/aid-admin"

# 3. State / config / logs.
if [[ "$KEEP_DATA" -eq 1 ]]; then
    ok "kept $DEPLOY_ROOT, $CONFIG_DIR, $LOG_DIR (--keep-data)"
else
    rm -rf "$DEPLOY_ROOT" "$CONFIG_DIR" "$LOG_DIR"
    ok "removed $DEPLOY_ROOT, $CONFIG_DIR, $LOG_DIR"
    # 4. Remove the dedicated system user (only the default aiddaemon; never a
    #    pre-existing --run-as login user).
    if [[ "$SERVICE_USER" == aiddaemon ]] && id aiddaemon >/dev/null 2>&1; then
        userdel aiddaemon 2>/dev/null || warn "userdel aiddaemon failed (processes still running?)"
        getent group aiddaemon >/dev/null && groupdel aiddaemon 2>/dev/null || true
        ok "removed system user 'aiddaemon'"
    elif [[ "$SERVICE_USER" != aiddaemon ]]; then
        warn "service ran as '$SERVICE_USER' (a --run-as user) — leaving that account intact"
    fi
fi

info "kept: the git clone (this repo) and all apt/Node packages"
ok "AID uninstalled."
