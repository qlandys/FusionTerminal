# Fusion API (auth)

Minimal auth service for the client UI.

## Quick start

```bash
cd server/api
JWT_SECRET="change-me" \
SMTP_HOST="smtp.example.com" \
SMTP_PORT="587" \
SMTP_USER="user@example.com" \
SMTP_PASS="app-password" \
SMTP_FROM="Fusion Terminal <user@example.com>" \
ADDR=":8686" \
go run .
```

## Environment

- `ADDR` (default `:8686`)
- `DB_PATH` (default `./fusion.db` JSON file)
- `JWT_SECRET` (required)
- `JWT_ISSUER` (default `fusionterminal`)
- `TOKEN_TTL` (default `168h`)
- `CODE_TTL` (default `10m`)
- `SMTP_HOST`
- `SMTP_PORT` (default `587`)
- `SMTP_USER`
- `SMTP_PASS`
- `SMTP_FROM` (optional)
- `SMTP_DISABLED=1` to log codes instead of sending email

## Endpoints

- `POST /auth/register` {`email`, `login`, `password`}
- `POST /auth/send_code` {`email_or_login`, `channel:"email"`}
- `POST /auth/verify_code` {`email_or_login`, `code`, `flow:"register"|"login"`}
- `POST /auth/login` {`email_or_login`, `password`}
- `GET /health`
