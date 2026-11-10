# mynetwork_mcp

A read-only stdio MCP server over the user's Blackbone **My Network** (`/member/network`). It calls the same
Supabase RPCs as the web page: `search_network_connections`, `get_network_orgs`, `get_network_facets` and
`get_network_person_profile`. It signs in as the user, so it only sees that user's own network.

Tools: `search_people`, `get_person`, `list_by_company`, `list_by_tag` (investor / founder / stealth),
`network_overview`.

Onurb's agent (`gateway/onurb_agent.py`) registers it as the `mynetwork` MCP server when this file exists.

## Config (outside the repo)

`~/.config/onurb/mynetwork.env` (mode 600; another path can be set with `MYNETWORK_ENV_FILE`):

```
MYNETWORK_SUPABASE_URL=...
MYNETWORK_SUPABASE_ANON_KEY=sb_publishable_...   # legacy anon JWT keys are disabled on the project
MYNETWORK_REFRESH_TOKEN=...                      # or MYNETWORK_EMAIL + MYNETWORK_PASSWORD
```

Refresh tokens rotate. The live session is cached in `~/.config/onurb/mynetwork_session.json`.

Test it with:

```
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"network_overview"}}' | python3 server.py
```
