# Server work: rate-limit desktop login + token auth (steps 6 & 7)

Copy-paste this as a prompt for yourself, a developer, or an AI coding agent with SSH access to production.

---

## Prompt

You have root access to the ProxyPi production server (`proxypi.co.uk`). Website app is Flask via gunicorn:

- Code: `/opt/proxypi/website.py` (service `proxypi.service`, binds `127.0.0.1:5000`, nginx terminates TLS)
- Internal RADIUS/API app: `/opt/proxypi/app.py` (service `radius-api.service`, port `5001`, **not public**)
- Desktop login endpoint already live: `POST /api/desktop/login` (JSON body `{email, password}`) → returns proxy host/ports + RADIUS username/password

### Goals

**Step 6 — Rate limit `/api/desktop/login` (do first, low risk)**  
Protect against password spraying / brute force.

Requirements:
1. Rate limit by **client IP** (use `X-Forwarded-For` first hop carefully, or `request.remote_addr` behind nginx ProxyFix — already configured).
2. Also rate limit by **email** (normalized lowercase) so distributed IPs cannot hammer one account forever.
3. Suggested limits (tunable constants):
   - **10 failed attempts / 15 minutes / IP**
   - **5 failed attempts / 15 minutes / email**
   - Optional: **30 total requests / 15 minutes / IP** (including successes)
4. On limit exceeded return **HTTP 429** JSON:  
   `{"ok": false, "error": "Too many login attempts. Try again later."}`
5. Use an in-memory dict with timestamps (acceptable for single-host gunicorn with multiple workers only if you use a shared store — prefer **Redis** if available, else **Flask-Limiter** / file lock / MySQL table). Prefer a solution that works with **gunicorn 4 workers** (not pure process-local memory unless you document that limits are per-worker).
6. Log rate-limit hits at WARNING with IP + email (never log password).
7. Do **not** break the existing desktop app: keep path `POST /api/desktop/login`, JSON request/response shape for success/failure.
8. Backup `website.py` before edit; `py_compile` then `systemctl restart proxypi.service`; smoke-test with curl.

**Step 7 — Short-lived token auth for desktop (do after step 6)**  
Stop requiring the website password on every “load proxy” if we add a second call later; improve security vs password-in-JSON for every fetch.

Design:
1. `POST /api/desktop/login` with email/password (rate-limited):
   - On success return:
     ```json
     {
       "ok": true,
       "email": "...",
       "token": "<opaque random token>",
       "expires_in": 3600,
       "proxy": { "host", "socks_port", "http_port", "username", "password" },
       "usage": { "total_mb", "threshold_mb", "status", "trial_status", "rotation_mode" }
     }
     ```
   - Keep returning `proxy` + `usage` on login so **current ProxyPiTester v1.1 still works** without changes.
2. Store tokens server-side (MySQL table or Redis):
   - `token_hash` (SHA-256 of token, never store raw if possible), `email`, `created_at`, `expires_at`, `revoked`
3. New endpoint for future app versions:  
   `GET /api/desktop/proxy` with header `Authorization: Bearer <token>`  
   - Validates token, returns fresh `proxy` + `usage` (so rotation can update node without re-entering password)
4. `POST /api/desktop/logout` with Bearer token → revoke token.
5. Token TTL default **1 hour**, refresh optional later.
6. Document for desktop app:
   - v1.1: ignore `token` field (backward compatible)
   - v1.2+: save token in memory/DPAPI; use Bearer for refresh; re-login on 401

### Security rules
- HTTPS only (already via nginx).
- Never log passwords or RADIUS secrets.
- Do not expose port 5001 publicly.
- Prefer not putting passwords in query strings (login must stay **POST body**).
- CORS: if kept, restrict or leave `*` only for this JSON API if needed; desktop WinHTTP does not need CORS.

### Acceptance tests
```bash
# Fail login
curl -sS -X POST https://proxypi.co.uk/api/desktop/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"nobody@example.com","password":"bad"}'
# Expect 401

# Burst fails → eventually 429
for i in $(seq 1 20); do
  curl -sS -o /dev/null -w "%{http_code}\n" -X POST https://proxypi.co.uk/api/desktop/login \
    -H 'Content-Type: application/json' \
    -d '{"email":"nobody@example.com","password":"bad"}'
done

# Valid login (use a real test account)
curl -sS -X POST https://proxypi.co.uk/api/desktop/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"TEST@proxypi.co.uk","password":"***"}'
# Expect 200, ok:true, proxy.host/ports/username/password present
# After step 7: also token + expires_in
```

### Deliverables
1. Patched `/opt/proxypi/website.py` (and schema/migration if token table)
2. Backup path noted
3. Services restarted, curl tests pasted
4. Short note for desktop team on backward compatibility

Support contact for users: **support@proxypi.co.uk**

---

## End of prompt
