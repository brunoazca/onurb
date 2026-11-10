#!/usr/bin/env python3
"""MyNetwork MCP: read-only search over the user's Blackbone "My Network" (stdio MCP).

My Network is the Blackbone web app's /member/network page. Its data comes from the
Supabase RPCs the web app calls (apps/web/src/services/networkExplorerService.ts):

  search_network_connections   free text + filters (companies, schools, locations, tags...)
  get_network_orgs             companies / schools / cities of the network (typeahead)
  get_network_facets           the network's overview: total, top companies/schools/locations
  get_network_person_profile   one person of the caller's network

Every RPC is SECURITY DEFINER and scoped by auth.uid(), so the server signs in as the
user (a Supabase user session, never the service-role key) and only sees *their* network.
All tools are reads.

Config (env vars, or the file in MYNETWORK_ENV_FILE, default ~/.config/onurb/mynetwork.env;
never commit it):
  MYNETWORK_SUPABASE_URL        the project's URL
  MYNETWORK_SUPABASE_ANON_KEY   the project's publishable key (sb_publishable_...;
                                legacy anon JWT keys are disabled on the project)
  and one of:
  MYNETWORK_REFRESH_TOKEN       a session refresh token (works for Google sign-in accounts)
  MYNETWORK_EMAIL + MYNETWORK_PASSWORD
The live session (access + rotated refresh token) is cached in
~/.config/onurb/mynetwork_session.json (mode 600). Stdlib only.
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

CONF_DIR = Path.home() / ".config" / "onurb"
ENV_FILE = Path(os.environ.get("MYNETWORK_ENV_FILE", CONF_DIR / "mynetwork.env"))
SESSION_FILE = CONF_DIR / "mynetwork_session.json"
TAGS = ["investor", "founder", "stealth"]


def _load_env_file():
    if not ENV_FILE.is_file():
        return
    for line in ENV_FILE.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


_load_env_file()
URL = os.environ.get("MYNETWORK_SUPABASE_URL", "").rstrip("/")
ANON = os.environ.get("MYNETWORK_SUPABASE_ANON_KEY", "")


class ToolError(Exception):
    pass


# ------------------------------------------------------------------ auth
def _http(method, path, body=None, token=None):
    if not URL or not ANON:
        raise ToolError("MyNetwork is not configured (MYNETWORK_SUPABASE_URL / _ANON_KEY missing).")
    req = urllib.request.Request(
        URL + path, method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"apikey": ANON, "Content-Type": "application/json",
                 # A publishable key (sb_publishable_...) is no JWT: Bearer only carries the user's.
                 **({"Authorization": f"Bearer {token}"} if token else {})})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            raw = r.read()
            return json.loads(raw) if raw else None
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:300]
        raise ToolError(f"HTTP {e.code} from MyNetwork: {detail}") from None


def _save_session(s):
    CONF_DIR.mkdir(parents=True, exist_ok=True)
    tmp = SESSION_FILE.with_suffix(".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump({"access_token": s["access_token"], "refresh_token": s["refresh_token"],
                   "expires_at": s.get("expires_at") or time.time() + s.get("expires_in", 3600)}, f)
    os.replace(tmp, SESSION_FILE)


def _token():
    s = None
    if SESSION_FILE.is_file():
        try:
            s = json.loads(SESSION_FILE.read_text())
        except ValueError:
            s = None
    if s and s.get("expires_at", 0) - 60 > time.time():
        return s["access_token"]
    # Refresh tokens rotate: the cached one is newer than the env's once a session exists.
    refresh = (s or {}).get("refresh_token") or os.environ.get("MYNETWORK_REFRESH_TOKEN")
    if refresh:
        try:
            new = _http("POST", "/auth/v1/token?grant_type=refresh_token", {"refresh_token": refresh})
            _save_session(new)
            return new["access_token"]
        except ToolError:
            if not os.environ.get("MYNETWORK_EMAIL"):
                raise ToolError("MyNetwork session expired; set a fresh MYNETWORK_REFRESH_TOKEN.")
    email, pw = os.environ.get("MYNETWORK_EMAIL"), os.environ.get("MYNETWORK_PASSWORD")
    if email and pw:
        new = _http("POST", "/auth/v1/token?grant_type=password", {"email": email, "password": pw})
        _save_session(new)
        return new["access_token"]
    raise ToolError("MyNetwork login missing: set MYNETWORK_REFRESH_TOKEN or MYNETWORK_EMAIL/PASSWORD.")


def rpc(name, args):
    return _http("POST", f"/rest/v1/rpc/{name}", args, token=_token())


# ------------------------------------------------------------------ shaping
DROP = {"photo_url", "company_logo", "school_logo", "logo", "logos", "suggestion_id"}


def _clean(v):
    """Strip nulls, empty values and image URLs so replies stay small."""
    if isinstance(v, dict):
        out = {k: _clean(x) for k, x in v.items() if k not in DROP}
        return {k: x for k, x in out.items() if x not in (None, "", [], {}, False)}
    if isinstance(v, list):
        return [_clean(x) for x in v]
    return v


def _person(p):
    name = " ".join(x for x in (p.get("first_name"), p.get("last_name")) if x) or "Unknown"
    out = {"person_id": p.get("person_id"), "name": name}
    for k in ("headline", "position", "company", "is_current", "location", "school", "degree",
              "seniority", "moved_recently", "fit_score", "linkedin", "same_school", "ex_colleague"):
        if p.get(k) not in (None, "", False):
            out[k] = p[k]
    skills = [s if isinstance(s, str) else (s or {}).get("name") for s in (p.get("skills") or [])]
    if skills:
        out["skills"] = [s for s in skills if s][:8]
    return out


def _resolve_org(kind, name):
    """A typed company/school name -> the org ids get_network_orgs knows (prefix match)."""
    rows = rpc("get_network_orgs", {"p_kind": kind, "p_query": name.strip(), "p_limit": 5}) or []
    return rows


def _search(query="", companies=None, schools=None, locations=None, tags=None, student=False,
            between_jobs=False, moved_only=False, sort="fit", limit=10, any_words=False):
    args = {"p_query": (query or "").strip(), "p_sort": sort if sort in ("fit", "recent", "az") else "fit",
            "p_limit": max(1, min(int(limit or 10), 50)), "p_any_words": bool(any_words),
            "p_near_misses": True}
    notes = []
    for kind, names, key in (("company", companies, "p_companies"), ("school", schools, "p_schools")):
        vals = []
        for n in names or []:
            hits = _resolve_org(kind, n)
            if not hits:
                notes.append(f"No {kind} named '{n}' in the network.")
                continue
            vals.append(hits[0]["value"])
            notes.append(f"{kind} '{n}' -> {hits[0]['name']} ({hits[0]['n']} people)")
        if vals:
            args[key] = vals
        elif names:
            return {"total": 0, "people": [], "notes": notes}
    if locations:
        args["p_locations"] = locations
    bad = [t for t in (tags or []) if t not in TAGS]
    if bad:
        raise ToolError(f"Unknown tag(s) {bad}; tags are {TAGS}.")
    if tags:
        args["p_tags"] = tags
    args.update(p_student=bool(student), p_between_jobs=bool(between_jobs), p_moved_only=bool(moved_only))
    r = rpc("search_network_connections", args) or {}
    out = {"total": r.get("total", 0), "capped": r.get("capped", False),
           "people": [_person(p) for p in r.get("people") or []]}
    if not out["people"] and r.get("near_misses"):
        out["near_misses"] = [{"without_filter": g.get("without"), "total": g.get("total"),
                               "people": [_person(p) for p in (g.get("people") or [])[:5]]}
                              for g in r["near_misses"]]
    if notes:
        out["notes"] = notes
    return _clean(out)


# ------------------------------------------------------------------ tools
STR_LIST = {"type": "array", "items": {"type": "string"}}
TOOLS = [
    {
        "name": "search_people",
        "description": (
            "Search the user's Blackbone My Network (their LinkedIn connections) like the My Network "
            "page does. `query` is free text over name, headline, title, company, school, skills and "
            "work area (all words required unless any_words). Optional filters: companies / schools "
            "(names; resolved to the network's organizations), locations (cities), tags (investor, "
            "founder, stealth), student, between_jobs, moved_only (changed jobs in the last 6 months). "
            "Returns people with person_id, name, headline, current role, location, school. Never read "
            "person_id aloud. If nobody matches, near_misses shows who matches all but one filter."),
        "inputSchema": {"type": "object", "properties": {
            "query": {"type": "string"}, "companies": STR_LIST, "schools": STR_LIST,
            "locations": STR_LIST, "tags": {"type": "array", "items": {"type": "string", "enum": TAGS}},
            "student": {"type": "boolean"}, "between_jobs": {"type": "boolean"},
            "moved_only": {"type": "boolean"}, "any_words": {"type": "boolean"},
            "sort": {"type": "string", "enum": ["fit", "recent", "az"]},
            "limit": {"type": "integer", "minimum": 1, "maximum": 50}}},
    },
    {
        "name": "get_person",
        "description": "Full My Network profile of one person (experience, education, skills) by person_id from search_people.",
        "inputSchema": {"type": "object", "properties": {"person_id": {"type": "string"}},
                        "required": ["person_id"]},
    },
    {
        "name": "list_by_company",
        "description": "People in the user's network who work or worked at a company (when: current, past or any).",
        "inputSchema": {"type": "object", "properties": {
            "company": {"type": "string"}, "when": {"type": "string", "enum": ["current", "past", "any"]},
            "limit": {"type": "integer", "minimum": 1, "maximum": 50}}, "required": ["company"]},
    },
    {
        "name": "list_by_tag",
        "description": "People in the user's network with a tag: investor, founder or stealth (stealth-mode startup).",
        "inputSchema": {"type": "object", "properties": {
            "tag": {"type": "string", "enum": TAGS}, "query": {"type": "string"},
            "limit": {"type": "integer", "minimum": 1, "maximum": 50}}, "required": ["tag"]},
    },
    {
        "name": "network_overview",
        "description": "Size of the user's network and its top companies, schools and locations.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def run_tool(name, a):
    if name == "search_people":
        return _search(a.get("query", ""), a.get("companies"), a.get("schools"), a.get("locations"),
                       a.get("tags"), a.get("student"), a.get("between_jobs"), a.get("moved_only"),
                       a.get("sort", "fit"), a.get("limit", 10), a.get("any_words"))
    if name == "get_person":
        r = rpc("get_network_person_profile", {"p_person_id": a["person_id"]})
        return _clean(r) if r else {"error": "Not in the user's network (or unknown id)."}
    if name == "list_by_company":
        hits = _resolve_org("company", a["company"])
        if not hits:
            return {"total": 0, "people": [], "notes": [f"No company named '{a['company']}' in the network."]}
        when = a.get("when", "any")
        args = {"p_query": "", "p_sort": "fit", "p_limit": max(1, min(int(a.get("limit", 15)), 50)),
                "p_companies": [hits[0]["value"]],
                "p_company_times": [None if when == "any" else when]}
        r = rpc("search_network_connections", args) or {}
        return _clean({"company": hits[0]["name"], "total": r.get("total", 0),
                       "people": [_person(p) for p in r.get("people") or []]})
    if name == "list_by_tag":
        return _search(a.get("query", ""), tags=[a["tag"]], limit=a.get("limit", 15))
    if name == "network_overview":
        r = rpc("get_network_facets", {}) or {}
        keep = {k: r.get(k) for k in ("total", "companies", "schools", "locations", "seniority", "moved")}
        return _clean(keep)
    raise ToolError(f"unknown tool {name}")


# ------------------------------------------------------------------ stdio JSON-RPC
def reply(msg_id, result=None, error=None):
    out = {"jsonrpc": "2.0", "id": msg_id}
    if error:
        out["error"] = {"code": -32603, "message": error}
    else:
        out["result"] = result
    sys.stdout.write(json.dumps(out) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        method, msg_id = msg.get("method"), msg.get("id")
        if msg_id is None:
            continue
        if method == "initialize":
            reply(msg_id, {"protocolVersion": msg.get("params", {}).get("protocolVersion", "2025-06-18"),
                           "capabilities": {"tools": {}},
                           "serverInfo": {"name": "mynetwork", "version": "0.1"}})
        elif method == "tools/list":
            reply(msg_id, {"tools": TOOLS})
        elif method == "tools/call":
            p = msg.get("params", {})
            try:
                res = run_tool(p.get("name"), p.get("arguments") or {})
                reply(msg_id, {"content": [{"type": "text", "text": json.dumps(res, ensure_ascii=False)}]})
            except Exception as e:  # noqa: BLE001 - report to the agent, keep serving
                reply(msg_id, {"content": [{"type": "text", "text": f"Tool error: {e}"}], "isError": True})
        elif method == "ping":
            reply(msg_id, {})
        else:
            reply(msg_id, error=f"method not found: {method}")


if __name__ == "__main__":
    main()
