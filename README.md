# timegrip-sync

Pulls my work schedule from Timegrip and syncs it to Google Calendar. Also calculates estimated pay and publishes a password-protected report to a web server so I always have an up-to-date picture of what I'm earning.

Runs every morning via GitHub Actions.

> Requires a Timegrip/Timeplan instance with ADFS SSO (SAML). The base URL is configured via environment variables.

## What it does

- Logs into Timegrip via SSO (ADFS/SAML) and fetches upcoming and recent shifts
- Diffs against the DB snapshot to figure out what changed, then creates/updates/deletes Google Calendar events accordingly
- Calculates gross pay, estimated net, supplements (evening, Saturday, Sunday), pension, AM-bidrag, ATP, etc.
- Archives a per-month HTML pay report in MySQL — once the pay date passes the month is locked and never overwritten
- Generates `gate.php` + `.htaccess` for FTP deployment; the PHP file authenticates with a cookie and serves reports straight from the DB

## Running locally

```
nix run . -- --dry-run
```

Skips all writes — just logs into Timegrip, connects to the DB and prints the timetable. Good for checking that everything is set up correctly.

```
nix run . -- --report /tmp/out/
```

Full run: syncs Google Calendar, updates the DB, and writes `gate.php` + `.htaccess` to the output directory. Use `--dry-run --report /tmp/out/` if you just want the report files without touching the calendar.

## Environment variables

Copy `.env.example` to `.env` and fill it in (`.env` is gitignored).

| Variable | What it's for |
|---|---|
| `TIMEGRIP_BASE_URL` | Base URL of your Timegrip instance |
| `EMAIL` / `PASSWORD` | Timegrip login credentials |
| `GOOGLE_CLIENT_ID` / `GOOGLE_CLIENT_SECRET` / `GOOGLE_REFRESH_TOKEN` | Google Calendar OAuth2 |
| `CALENDAR_NAME` | Name of the Google Calendar to sync into |
| `REPORT_PASSWORD` / `REPORT_SALT` | Password and salt for the web report |
| `SITE_LABEL` | Branding shown on the report login page |
| `DB_HOST` / `DB_USER` / `DB_PASSWORD` / `DB_DATABASE` | MySQL — required |
| `SYNC_AHEAD_WEEKS` | How many weeks ahead to sync (default: 12) |
| `SYNC_LOOKBACK_WEEKS` | How far back shifts can still be changed (default: 4) |

Wage config (`HOURLY_RATE`, `EVENING_SUPPLEMENT`, `TAX_PCT`, etc.) are all required — see `--help` for the full list or just copy `.env.example`.

## Building

Needs [Nix](https://nixos.org/download). Everything else is handled by the flake.

```
nix build
```

## Database

MySQL is required. Tables are created automatically on the first run.

- `shifts` — live snapshot of every shift; gets updated every run so you always know what's current
- `shift_changes` — immutable log of every created/updated/deleted event
- `sync_runs` — one row per execution with counts and a reference to the changes it made
- `pay_periods` — one row per pay month with aggregates and full HTML; locked once the pay date has passed

## TODO
- notifications through webhooks or other form af push notifications on schedule changes or other events.
- Make CLI tool less usecase specific, support other login types besides ADFS SSO/SAML.
- Implement more settings user defineable calculations of wage in terms salary supplement of holidays or hours of the day as well as an advanced way of setting your own calculations of wage.
- Improve SoC by decoupling DB handling and report generation, making the core tool modular and adaptable for specific use cases.
- Visualisation, in form of the output report, of the shift changes log.
- Support for other calendar backends (Outlook, Apple Calendar).
- `.md` report output format.
- `.pdf` report output format.
- Tools for tax doing your taxes ([https://skat.dk/](https://skat.dk/))
- Export reports to CSV/Excel for easier accounting and tax tracking.
- automatically flag weeks where hours exceed standard limits and calculate the projected overtime pay.
- Ability to handle multiple TimeGrip accounts if working multiple jobs that use the same system.
- Implement reverse sync for time-off requests triggered by specific event tags/types in the user's personal calendar.
