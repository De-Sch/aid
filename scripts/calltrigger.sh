#!/usr/bin/env bash
# calltrigger.sh — fire synthetic /call phone events at a running AID2.0 daemon.
#
# Mirrors the five /call wire shapes documented in docs/02-integrating-call-api.md,
# so you can drive a call lifecycle by hand without a phone system in the loop.
#
# Usage:
#   scripts/calltrigger.sh -<flags> <callid> [caller] [dialed] [options]
#
# Flags (combine in any order, e.g. -iath):
#   i   Incoming Call   needs callid + caller + dialed
#   a   Accepted Call   needs callid + caller + dialed   (+ optional user via -u)
#   t   Transfer Call   needs callid + newuser           (no phone numbers)
#   h   Hangup          needs callid + caller
#
# GAP: the fifth wire shape (Outgoing Call) is NOT covered here — this tool
# drives the inbound lifecycle only. For an Outgoing payload, POST the shape
# documented in docs/02-integrating-call-api.md directly.
#
# Positional args after the flag token:
#   <callid>   required — the call id all selected events share
#   [caller]   the calling party number  (JSON "remote"); default $AID_CALLER
#   [dialed]   the dialed/called number   (JSON "dialed"); default $AID_DIALED
#
# Options:
#   -c <num>    caller number  (overrides positional / default)
#   -d <num>    dialed number  (overrides positional / default)
#   -u <user>   accepted-call user handle  (Accepted only; optional)
#   -n <user>   transfer target handle     (Transfer only; required for -t)
#   -H <host>   daemon host   (default $AID_HOST or 127.0.0.1)
#   -P <port>   daemon port   (default $AID_PORT or 8088)
#   -s <sec>    sleep between events (default 0.2) — keeps per-callid ordering
#   -N          dry run: print the JSON payloads, do not POST
#   -v          verbose curl
#
# Whatever the letter order, events fire in lifecycle order i -> a -> t -> h so
# the daemon's per-callid mailbox sees them correctly ordered.
#
# Examples:
#   scripts/calltrigger.sh -i 1001 +4915112345678 +4930222
#   scripts/calltrigger.sh -iah 1001 +4915112345678 +4930222 -u alice
#   scripts/calltrigger.sh -t 1001 -n bob
#   AID_HOST=192.168.178.54 scripts/calltrigger.sh -iath 1001 +49151 +49302 -n bob

set -euo pipefail

prog=$(basename "$0")

# ---- defaults -------------------------------------------------------------
host=${AID_HOST:-127.0.0.1}
port=${AID_PORT:-8088}
caller=${AID_CALLER:-+4915112345678}
dialed=${AID_DIALED:-+493020220000}
user=""
newuser=""
sleep_between=0.2
dry_run=0
curl_verbose=""

do_i=0 do_a=0 do_t=0 do_h=0
flags_seen=0

positionals=()

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

die() { echo "$prog: error: $*" >&2; exit 2; }

# ---- argument parsing -----------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --help) usage 0 ;;
        -c) caller="${2:?-c needs a value}"; shift ;;
        -d) dialed="${2:?-d needs a value}"; shift ;;
        -u) user="${2:?-u needs a value}"; shift ;;
        -n) newuser="${2:?-n needs a value}"; shift ;;
        -H) host="${2:?-H needs a value}"; shift ;;
        -P) port="${2:?-P needs a value}"; shift ;;
        -s) sleep_between="${2:?-s needs a value}"; shift ;;
        -N) dry_run=1 ;;
        -v) curl_verbose="-v" ;;
        --) shift; while [[ $# -gt 0 ]]; do positionals+=("$1"); shift; done; break ;;
        -*)
            # A flag group: every char must be one of i/a/t/h.
            grp="${1#-}"
            [[ -n "$grp" ]] || die "empty flag '-'"
            for (( j=0; j<${#grp}; j++ )); do
                case "${grp:$j:1}" in
                    i) do_i=1 ;;
                    a) do_a=1 ;;
                    t) do_t=1 ;;
                    h) do_h=1 ;;
                    *) die "unknown flag '-${grp:$j:1}' (valid: i a t h)" ;;
                esac
            done
            flags_seen=1 ;;
        *) positionals+=("$1") ;;
    esac
    shift
done

[[ "$flags_seen" -eq 1 ]] || { echo "$prog: no event flag given." >&2; usage 2; }

# positionals: callid [caller] [dialed]
callid="${positionals[0]:-}"
[[ -n "$callid" ]] || die "callid is required (first positional argument)"
[[ -n "${positionals[1]:-}" ]] && caller="${positionals[1]}"
[[ -n "${positionals[2]:-}" ]] && dialed="${positionals[2]}"

# ---- per-event requirement checks -----------------------------------------
if (( do_i || do_a || do_h )) && [[ -z "$caller" ]]; then
    die "caller number required for -i/-a/-h (positional 2, or -c <num>)"
fi
if (( do_i || do_a )) && [[ -z "$dialed" ]]; then
    die "dialed number required for -i/-a (positional 3, or -d <num>)"
fi
if (( do_t )) && [[ -z "$newuser" ]]; then
    die "transfer (-t) requires a target handle: -n <user>"
fi

url="http://${host}:${port}/call"

# ---- JSON builders (match calls.py field names exactly) -------------------
json_escape() { local s=$1; s=${s//\\/\\\\}; s=${s//\"/\\\"}; printf '%s' "$s"; }

post() {
    local label=$1 payload=$2
    echo "==> ${label}: ${payload}"
    if (( dry_run )); then return 0; fi
    local code
    code=$(curl $curl_verbose -sS -o /dev/null -w '%{http_code}' \
        -X POST -H 'Content-Type: application/json' \
        --data "$payload" "$url") || die "curl failed posting to $url (is the daemon up?)"
    echo "    HTTP ${code}  (${url})"
    [[ "$code" == "202" ]] || echo "    !! expected 202 Accepted" >&2
}

cid=$(json_escape "$callid")
rem=$(json_escape "$caller")
dia=$(json_escape "$dialed")

# Fire in lifecycle order regardless of flag order.
if (( do_i )); then
    post "Incoming Call" \
        "{\"event\": \"Incoming Call\", \"remote\": \"${rem}\", \"callid\": \"${cid}\", \"dialed\": \"${dia}\"}"
    sleep "$sleep_between"
fi

if (( do_a )); then
    if [[ -n "$user" ]]; then
        usr=$(json_escape "$user")
        post "Accepted Call" \
            "{\"event\": \"Accepted Call\", \"callid\": \"${cid}\", \"remote\": \"${rem}\", \"dialed\": \"${dia}\", \"user\": \"${usr}\"}"
    else
        post "Accepted Call" \
            "{\"event\": \"Accepted Call\", \"callid\": \"${cid}\", \"remote\": \"${rem}\", \"dialed\": \"${dia}\"}"
    fi
    sleep "$sleep_between"
fi

if (( do_t )); then
    nu=$(json_escape "$newuser")
    post "Transfer Call" \
        "{\"event\": \"Transfer Call\", \"callid\": \"${cid}\", \"newuser\": \"${nu}\"}"
    sleep "$sleep_between"
fi

if (( do_h )); then
    post "Hangup" \
        "{\"event\": \"Hangup\", \"callid\": \"${cid}\", \"remote\": \"${rem}\"}"
fi
