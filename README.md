# timegrip-sync

Designet to pull work schedules from Timegrip/Timeplan and sync it with google calender. Additionally it calculates estimated pay based on hourly wages and creates a report which can be automatically published to a web server to always be up to date with expected pay. Supports database storage to log changes in shifts.

In this specific repo it is set to run every morning with Github Actions.

> Requires a Timegrip/Timeplan instance with ADFS SSO (SAML). The base URL and ADFS host are configured via environment variables.

## What it does

- Logs into Timegrip via SSO (ADFS/SAML), fetches upcoming and recent shifts
- Creates/updates/deletes Google Calendar events to match
- Calculates gross pay, estimated net, supplements (evening, Saturday, Sunday), pension, AM-bidrag, ATP, etc.
- Archives a per-month HTML report in MySQL — once the pay date passes, that month is locked and never overwritten
- Generates `gate.php` + `.htaccess` for FTP deployment; the PHP file serves reports straight from the DB with cookie auth

## Running locally

```
nix run . -- --dry-run --report /tmp/out/
```

`--dry-run` skips Google Calendar writes. `--report DIR` writes `gate.php` + `.htaccess` to the directory (or a plain `report.html` if `REPORT_PASSWORD` isn't set). Without `--save`, nothing touches the database.

```
nix run . -- --report /tmp/out/ --save
```

Adds DB writes: shift changelog + period archive.

## Environment variables

Copy `.env.example` to `.env` and fill it in (`.env` is gitignored).

| Variable | What it's for |
|---|---|
| `TIMEGRIP_BASE_URL` | Base URL of your Timegrip instance |
| `SALLING_EMAIL` / `SALLING_PASSWORD` | Timegrip login credentials |
| `GOOGLE_CLIENT_ID` / `GOOGLE_CLIENT_SECRET` / `GOOGLE_REFRESH_TOKEN` | Google Calendar OAuth2 |
| `CALENDAR_NAME` | Name of the Google Calendar to sync into |
| `REPORT_PASSWORD` / `REPORT_SALT` | Password and salt for the web report |
| `SITE_LABEL` | Branding label shown on the login page |
| `DB_HOST` / `DB_USER` / `DB_PASSWORD` / `DB_DATABASE` | MySQL (optional, needed for `--save`) |
| `SYNC_AHEAD_WEEKS` | How many weeks ahead to sync (default: 12) |
| `SYNC_LOOKBACK_WEEKS` | How many weeks back to allow changes (default: 4) |

Wage config (`HOURLY_RATE`, `EVENING_SUPPLEMENT`, `SATURDAY_SUPPLEMENT`, `TAX_PCT`, etc.) are **required** env vars — see `--help` for the full list or copy from `.env.example`.

## Building

Needs [Nix](https://nixos.org/download). Everything else is handled by the flake.

```
nix build
```

## Database schema

Three tables, all created automatically on first `--save` run:

- `shifts` — live snapshot of every known shift
- `shift_changes` — immutable changelog (created/updated/deleted)
- `sync_runs` — one row per execution with counts and a JSON array of change IDs
- `pay_periods` — one row per pay month with structured aggregates + full HTML; `locked=1` once pay date has passed

> ## TODO 
> - Notifications through webhooks to various services on schedule changes, or when nearing a shift 
> - Visualisation of shift changes data, to easy be able to inspect when changes were made to the schedule and what changes
> - Rework CLI application to work more generally with any TimeGrip/Timeplan instance
> - Support for different calenders, Outlook, Apple Calender.
> - .md report output format
