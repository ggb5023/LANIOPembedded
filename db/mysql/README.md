# LAN Chat MySQL Schema

Apply the v1 schema with:

```powershell
mysql -uroot -p < db/mysql/schema.sql
```

The schema intentionally stores `password_hash` and `password_salt` only. It does not include `password_plain`, `contacts`, `offline_messages`, or `client_acks`.
