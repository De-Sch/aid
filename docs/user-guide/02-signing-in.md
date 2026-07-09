# 2. Signing in & passwords

[← Installation](01-installation.md) · [Back to User Guide](README.md)

Open the dashboard in a browser at `http://<your-host>:<port>/` and the sign-in
screen is the first thing you'll see.

## 2.1 Sign in

![The sign-in screen](img/login.png)

1. **Username** — your operator login. It's the same name as your OpenProject login;
   that's how AID knows which calls and tickets are yours.
2. **Password** — your dashboard password.
3. **Sign in** — takes you through to the dashboard.

Get the details right and you land straight on the ticket board
([Using the dashboard](03-using-the-dashboard.md)). To leave, there's a **Log out**
button in the top-right corner.

## 2.2 Forgot your password? Use the recovery key

There's no email reset here. Instead there's a recovery key — the master password
your administrator set during [installation](01-installation.md) — and it can set a
new password for *any* account.

To reset a password:

1. On the sign-in screen, type in the username whose password you want to reset.
2. In the password box, type the recovery key rather than a password.
3. Click **Sign in**.

![Entering the recovery key on the sign-in screen](img/masterkey-login.png)

So you're only filling in two things on the sign-in screen:

1. **Username** — the account whose password you're resetting.
2. **Password box** — the recovery key goes here, in place of a normal password (it
   shows as dots, just like any password would).

Because the recovery key was recognised, you're sent to a **Set a new password**
screen instead of the dashboard:

![Setting a new password](img/reset-password.png)

1. **New password** — pick a new password (at least 8 characters).
2. **Confirm password** — type it again.
3. **Save new password** — and the account is updated.

You're dropped back on the sign-in screen; log in with the *new* password. The
recovery key itself doesn't change, so you can use it again whenever you need to.

> **Keep the recovery key safe.** Anyone who has it can reset any operator's
> password. Treat it like an administrator credential — store it in your password
> manager, not on a sticky note.

## 2.3 Adding more operators

New operator accounts are created by the administrator on the server, and each
dashboard login has to match an OpenProject login. Once an account exists, that
operator signs in exactly as above — and if they ever forget their password, the
recovery-key flow above gets them back in.

---

Next: [Using the dashboard →](03-using-the-dashboard.md)
