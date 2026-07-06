#!/usr/bin/env python3
"""
Sync Timegrip timetable to a dedicated Google Calendar.

Setup:
  1. Create a Google Cloud project, enable Calendar API, download OAuth2
     credentials as credentials.json next to this script.
  2. Set SALLING_EMAIL and SALLING_PASSWORD environment variables.
  3. Run once — browser opens for Google OAuth consent, token.json is saved.

Subsequent runs are fully automated.
"""

import os
import re
import sys
import requests
from datetime import datetime, timedelta
from pathlib import Path

from dotenv import load_dotenv
from googleapiclient.discovery import build
from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from google.auth.transport.requests import Request

load_dotenv()

# ── Config ────────────────────────────────────────────────────────────────────

TG_BASE = "https://example.timeplan-software.com"
WEEKS_AHEAD = 12
TIMEZONE = "Europe/Copenhagen"
CALENDAR_NAME = "Work"
SCOPES = ["https://www.googleapis.com/auth/calendar"]

# Absence type_captions that count as "day off, guaranteed no work"
VACATION_TYPES = {"ferie", "fritimer"}

# Absence type_captions to silently skip (no calendar event)
SKIP_TYPES = {"ph-reduktion", "ph-reduction"}

# ── Timegrip auth ─────────────────────────────────────────────────────────────

def _adfs_session(email, password):
    session = requests.Session()
    session.headers["User-Agent"] = (
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
    )

    resp = session.get(
        f"{TG_BASE}/sso/auth/login",
        params={"returnUrl": f"{TG_BASE}/?SSOSuccessful"},
        timeout=15,
        allow_redirects=True,
    )
    if "sts.dsg.dk" not in resp.url:
        raise RuntimeError(f"Expected ADFS redirect, got: {resp.url}")

    action_match = re.search(r'action="(/adfs/ls/[^"]+)"', resp.text)
    if not action_match:
        raise RuntimeError("Could not find ADFS form action")
    form_action = "https://sts.dsg.dk" + action_match.group(1).replace("&amp;", "&")

    resp2 = session.post(
        form_action,
        data={
            "UserName": email,
            "Password": password,
            "AuthMethod": "FormsAuthentication",
            "Kmsi": "true",
        },
        timeout=15,
        allow_redirects=True,
    )

    saml_match = re.search(
        r'name=["\']SAMLResponse["\'][^>]+value=["\']([^"\']+)["\']', resp2.text
    ) or re.search(
        r'value=["\']([^"\']+)["\'][^>]+name=["\']SAMLResponse["\']', resp2.text
    )
    if not saml_match:
        err = re.search(r'<li[^>]*>\s*(.*?)\s*</li>', resp2.text)
        msg = err.group(1) if err else "unknown (wrong credentials?)"
        raise RuntimeError(f"ADFS auth failed: {msg}")

    relay_match = re.search(
        r'name=["\']RelayState["\'][^>]+value=["\']([^"\']*)["\']', resp2.text
    ) or re.search(
        r'value=["\']([^"\']*)["\'][^>]+name=["\']RelayState["\']', resp2.text
    )
    acs_match = re.search(r'<form[^>]+action=["\']([^"\']+)["\']', resp2.text)
    acs_url = acs_match.group(1) if acs_match else f"{TG_BASE}/sso/Saml/AssertionConsumerService"

    session.post(
        acs_url,
        data={
            "SAMLResponse": saml_match.group(1),
            "RelayState": relay_match.group(1) if relay_match else "",
        },
        timeout=15,
        allow_redirects=True,
    )

    check = session.get(f"{TG_BASE}/webapi/?func=isAuthenticated", timeout=10)
    if check.status_code != 200:
        raise RuntimeError("Session check failed after ADFS auth")

    return session

def timegrip_session():
    email = os.getenv("SALLING_EMAIL")
    password = os.getenv("SALLING_PASSWORD")
    if not email or not password:
        print("Error: set SALLING_EMAIL and SALLING_PASSWORD")
        sys.exit(1)
    print("  Authenticating with ADFS…")
    session = _adfs_session(email, password)
    print("  ✓ Timegrip session ready")
    return session

# ── Timegrip data ─────────────────────────────────────────────────────────────

def fetch_timetable(session):
    from_date = datetime.now().strftime("%Y-%m-%dT00:00:00.000")
    to_date = (datetime.now() + timedelta(weeks=WEEKS_AHEAD)).strftime("%Y-%m-%d")
    r = session.get(
        f"{TG_BASE}/webapi/?func=MyWorktimes",
        params={"from_date": from_date, "to_date": to_date},
        timeout=15,
    )
    r.raise_for_status()
    return r.json(), from_date[:10], to_date

def fetch_function_names(session):
    r = session.get(f"{TG_BASE}/webapi/?func=LoadSetting", timeout=15)
    r.raise_for_status()
    funcs = r.json()["LoadSetting"][0]["jobFunctions"]
    return {str(f["id"]): f["name"] for f in funcs}

def _parse_time(iso):
    m = re.search(r'T(\d{2}:\d{2})', iso)
    return m.group(1) if m else "?"

def _parse_date(iso):
    if "T" in iso:
        return datetime.strptime(iso[:10], "%Y-%m-%d").date()
    return datetime.strptime(iso, "%d-%m-%Y").date()

def build_desired_events(timetable, func_map):
    """
    Returns dict of timegrip_id → Google Calendar event body.
    Skips PH-reduction and similar. Treats Ferie + Fritimer as all-day off events.
    """
    events = {}

    for week in timetable["weeks"]:
        for day in week.get("days", []):
            day_date = _parse_date(day["date"])

            for shift in day.get("shifts", []):
                absence = shift.get("absence")
                caption = absence.get("type_caption", "") if absence else ""
                caption_lc = caption.lower()

                # Silently skip PH-reduction etc.
                if any(skip in caption_lc for skip in SKIP_TYPES):
                    continue

                # Ferie / Fritimer → all-day event
                if absence and (not shift.get("worktime_id") or any(v in caption_lc for v in VACATION_TYPES)):
                    date_str = day_date.isoformat()
                    tid = f"absence_{date_str}_{caption_lc}"
                    events[tid] = {
                        "summary": f"{caption} — guaranteed no work",
                        "start": {"date": date_str},
                        "end": {"date": date_str},
                        "extendedProperties": {
                            "private": {"timegrip_id": tid, "source": "timegrip"}
                        },
                    }
                    continue

                # Normal shift
                worktime_id = shift.get("worktime_id")
                if not worktime_id:
                    continue

                start_t = _parse_time(shift["start_time"])
                end_t = _parse_time(shift["end_time"])
                date_str = day_date.isoformat()

                duration_mins = shift.get("duration", 0)
                h, m = duration_mins // 60, duration_mins % 60
                duration_str = f"{h}h {m}m" if m else f"{h}h"

                pause = shift.get("start_pause") or shift.get("actual_pause") or 0
                func_id = str(shift["details"][0]["function_id"]) if shift.get("details") else None
                role = func_map.get(func_id, "") if func_id else ""

                desc_parts = [duration_str]
                if pause:
                    desc_parts.append(f"{pause}m break")
                if role:
                    desc_parts.append(role)

                events[str(worktime_id)] = {
                    "summary": f"Netto — {start_t}–{end_t}",
                    "description": " · ".join(desc_parts),
                    "start": {"dateTime": f"{date_str}T{start_t}:00", "timeZone": TIMEZONE},
                    "end": {"dateTime": f"{date_str}T{end_t}:00", "timeZone": TIMEZONE},
                    "extendedProperties": {
                        "private": {
                            "timegrip_id": str(worktime_id),
                            "source": "timegrip",
                        }
                    },
                }

    return events

# ── Google Calendar ───────────────────────────────────────────────────────────

def google_service():
    client_id = os.getenv("GOOGLE_CLIENT_ID")
    client_secret = os.getenv("GOOGLE_CLIENT_SECRET")
    refresh_token = os.getenv("GOOGLE_REFRESH_TOKEN")

    # Path A: all three env vars set → headless, no files needed (GitHub Actions)
    if client_id and client_secret and refresh_token:
        creds = Credentials(
            token=None,
            refresh_token=refresh_token,
            token_uri="https://oauth2.googleapis.com/token",
            client_id=client_id,
            client_secret=client_secret,
            scopes=SCOPES,
        )
        creds.refresh(Request())
        return build("calendar", "v3", credentials=creds)

    # Path B: token.json already exists from a previous local login
    token_path = Path("token.json")
    creds_path = Path("credentials.json")
    creds = None

    if token_path.exists():
        creds = Credentials.from_authorized_user_file(str(token_path), SCOPES)

    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            creds.refresh(Request())
        else:
            # Path C: first-time local login via browser
            if not creds_path.exists():
                print("Error: no Google credentials found.")
                print("Either set GOOGLE_CLIENT_ID + GOOGLE_CLIENT_SECRET + GOOGLE_REFRESH_TOKEN")
                print("or place credentials.json next to this script for first-time login.")
                sys.exit(1)
            flow = InstalledAppFlow.from_client_secrets_file(str(creds_path), SCOPES)
            creds = flow.run_local_server(port=0)
        token_path.write_text(creds.to_json())
        if not refresh_token:
            print(f"\n  Refresh token obtained. Add to .env:")
            print(f"  GOOGLE_REFRESH_TOKEN={creds.refresh_token}\n")

    return build("calendar", "v3", credentials=creds)

def get_or_create_calendar(service):
    result = service.calendarList().list().execute()
    for cal in result.get("items", []):
        if cal["summary"] == CALENDAR_NAME:
            return cal["id"]

    cal = service.calendars().insert(body={
        "summary": CALENDAR_NAME,
        "timeZone": TIMEZONE,
    }).execute()
    print(f"  Created calendar '{CALENDAR_NAME}'")
    return cal["id"]

def fetch_existing_events(service, calendar_id, from_date, to_date):
    existing = {}
    page_token = None
    while True:
        resp = service.events().list(
            calendarId=calendar_id,
            timeMin=f"{from_date}T00:00:00Z",
            timeMax=f"{to_date}T23:59:59Z",
            privateExtendedProperty="source=timegrip",
            singleEvents=True,
            pageToken=page_token,
        ).execute()
        for ev in resp.get("items", []):
            tid = ev.get("extendedProperties", {}).get("private", {}).get("timegrip_id")
            if tid:
                existing[tid] = ev
        page_token = resp.get("nextPageToken")
        if not page_token:
            break
    return existing

def _events_differ(existing, desired):
    if existing.get("summary") != desired.get("summary"):
        return True
    if existing.get("description", "") != desired.get("description", ""):
        return True
    for key in ("start", "end"):
        e, d = existing.get(key, {}), desired.get(key, {})
        if e.get("dateTime") != d.get("dateTime"):
            return True
        if e.get("date") != d.get("date"):
            return True
    return False

def sync_events(service, calendar_id, desired, existing):
    created = updated = deleted = 0

    for tid, body in desired.items():
        if tid not in existing:
            service.events().insert(calendarId=calendar_id, body=body).execute()
            created += 1
        elif _events_differ(existing[tid], body):
            merged = {**existing[tid], **body}
            service.events().update(
                calendarId=calendar_id,
                eventId=existing[tid]["id"],
                body=merged,
            ).execute()
            updated += 1

    for tid, ev in existing.items():
        if tid not in desired:
            service.events().delete(calendarId=calendar_id, eventId=ev["id"]).execute()
            deleted += 1

    return created, updated, deleted

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    print("Timegrip → Google Calendar\n")

    print("Timegrip:")
    session = timegrip_session()

    print("Fetching timetable…")
    timetable, from_date, to_date = fetch_timetable(session)
    func_map = fetch_function_names(session)
    desired = build_desired_events(timetable, func_map)
    print(f"  {len(desired)} events to sync ({from_date} → {to_date})")

    print("\nGoogle Calendar:")
    service = google_service()
    calendar_id = get_or_create_calendar(service)

    print("Fetching existing events…")
    existing = fetch_existing_events(service, calendar_id, from_date, to_date)
    print(f"  {len(existing)} existing managed events")

    print("Syncing…")
    created, updated, deleted = sync_events(service, calendar_id, desired, existing)

    print(f"\n✓ Done — {created} created · {updated} updated · {deleted} deleted")

if __name__ == "__main__":
    main()
