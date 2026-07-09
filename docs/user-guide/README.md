# AID2.0 — User Guide

This guide is for the people who run and use AID2.0 day to day:

- the **administrator** who installs it on a server, and
- the **operators** who watch calls turn into tickets on the dashboard.

It assumes the ticket system (OpenProject) and the address book (CardDAV/DaviCal)
are already up on your network. For the developer-facing material — the HTTP API,
the plugin ABI, the configuration internals — head to the
[Core API & Integration Guide](../README.md).

## Contents

| # | Chapter | For |
|---|---------|-----|
| 1 | [Installation](01-installation.md) | the administrator — install the daemon with the setup wizard |
| 2 | [Signing in & passwords](02-signing-in.md) | everyone — logging in, and resetting a password with the recovery key |
| 3 | [Using the dashboard](03-using-the-dashboard.md) | operators — the live-call banner, the ticket list, commenting and closing |

## What AID2.0 does, in one picture

When a call comes in, AID2.0 looks the caller up, routes the call to the right
project, and creates or updates a ticket — and the operator handling the call sees
it happen live on the dashboard:

> A phone rings → a ticket appears on the board → the operator answers → the ticket
> shows **who's on the call right now** → when the call ends, the ticket stays open
> for follow-up until someone closes it.

The rest of this guide walks through each of those pieces.
