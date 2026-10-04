from pathlib import Path


def rep(text, old, new, label, count=1):
    n = text.count(old)
    if n != count:
        raise SystemExit(f"{label}: expected {count} match(es), got {n}")
    return text.replace(old, new, count)


# OSCam Newcamd server: validate login packet boundaries and separate
# unknown-user, bad-password and account-policy failures.  The old code used
# strlen() on untrusted packet data and then inferred "user does not exist"
# from the loop iterator, which misclassified bad passwords and malformed
# requests.  Do not log password hashes.
p = Path("third_party/oscam-mini/module-newcamd.c")
s = p.read_text()

s = rep(
    s,
    "\tstruct s_auth *account;\n",
    "\tstruct s_auth *account, *matched_account = NULL;\n",
    "matched account declaration",
)

old_parse = """\ti = process_input(mbuf, sizeof(mbuf), cfg.cmaxidle);\n\tif(i > 0)\n\t{\n\t\tif(mbuf[2] != MSG_CLIENT_2_SERVER_LOGIN)\n\t\t{\n\t\t\tcs_log_dbg(D_CLIENT, \"expected MSG_CLIENT_2_SERVER_LOGIN (%02X), received %02X\",\n\t\t\t\t\t\t  MSG_CLIENT_2_SERVER_LOGIN, mbuf[2]);\n\t\t\treturn -1;\n\t\t}\n\t\tusr = mbuf + 5;\n\t\tpwd = usr + strlen((char *)usr) + 1;\n\t}\n\telse\n\t{\n\t\tcs_log_dbg(D_CLIENT, \"bad client login request\");\n\t\treturn -1;\n\t}\n"""
new_parse = """\ti = process_input(mbuf, sizeof(mbuf), cfg.cmaxidle);\n\tif(i > 0)\n\t{\n\t\tif(i <= 5 || mbuf[2] != MSG_CLIENT_2_SERVER_LOGIN)\n\t\t{\n\t\t\tcs_log_dbg(D_CLIENT, \"expected valid MSG_CLIENT_2_SERVER_LOGIN (%02X), received cmd=%02X len=%d\",\n\t\t\t\t\t\t  MSG_CLIENT_2_SERVER_LOGIN, (i > 2) ? mbuf[2] : 0, i);\n\t\t\treturn -1;\n\t\t}\n\n\t\tuchar *login_end = mbuf + i;\n\t\tusr = mbuf + 5;\n\t\tuchar *usr_end = (uchar *)memchr(usr, 0, (size_t)(login_end - usr));\n\t\tif(!usr_end || usr_end == usr)\n\t\t{\n\t\t\tcs_log(\"rejecting malformed newcamd login: invalid username field\");\n\t\t\treturn -1;\n\t\t}\n\n\t\tpwd = usr_end + 1;\n\t\tif(pwd >= login_end)\n\t\t{\n\t\t\tcs_log(\"rejecting malformed newcamd login: missing password field\");\n\t\t\treturn -1;\n\t\t}\n\t\tuchar *pwd_end = (uchar *)memchr(pwd, 0, (size_t)(login_end - pwd));\n\t\tif(!pwd_end || pwd_end == pwd)\n\t\t{\n\t\t\tcs_log(\"rejecting malformed newcamd login: invalid password field\");\n\t\t\treturn -1;\n\t\t}\n\t}\n\telse\n\t{\n\t\tcs_log_dbg(D_CLIENT, \"bad client login request\");\n\t\treturn -1;\n\t}\n"""
s = rep(s, old_parse, new_parse, "bounded login parsing")

old_auth = """\tfor(ok = 0, account = cfg.account; (usr) && (account) && (!ok); account = account->next)\n\t{\n\t\tcs_log_dbg(D_CLIENT, \"account->usr=%s\", account->usr);\n\t\tif(strcmp((char *)usr, account->usr) == 0)\n\t\t{\n\t\t\t__md5_crypt(ESTR(account->pwd), \"$1$abcdefgh$\", (char *)passwdcrypt);\n\t\t\tcs_log_dbg(D_CLIENT, \"account->pwd=%s\", passwdcrypt);\n\t\t\tif(strcmp((char *)pwd, (const char *)passwdcrypt) == 0)\n\t\t\t{\n\t\t\t\tcl->crypted = 1;\n\t\t\t\tchar e_txt[20];\n\t\t\t\tsnprintf(e_txt, 20, \"%s:%d\", \"newcamd\", cfg.ncd_ptab.ports[cl->port_idx].s_port);\n\t\t\t\tif((rc = cs_auth_client(cl, account, e_txt)) == 2)\n\t\t\t\t{\n\t\t\t\t\tcs_log(\"hostname or ip mismatch for user %s (%s)\", usr, client_name);\n\t\t\t\t\tbreak;\n\t\t\t\t}\n\t\t\t\telse if(rc != 0)\n\t\t\t\t{\n\t\t\t\t\tcs_log(\"account is invalid for user %s (%s)\", usr, client_name);\n\t\t\t\t\tbreak;\n\t\t\t\t}\n\t\t\t\telse\n\t\t\t\t{\n\t\t\t\t\tcs_log(\"user %s authenticated successfully (%s)\", usr, client_name);\n\t\t\t\t\tok = 1;\n\t\t\t\t\tbreak;\n\t\t\t\t}\n\t\t\t}\n\t\t\telse\n\t\t\t\t{ cs_log(\"user %s is providing a wrong password (%s)\", usr, client_name); }\n\t\t}\n\t}\n\n\tif(!ok && !account)\n\t{\n\t\tcs_log(\"user %s is trying to connect but doesnt exist ! (%s)\", usr, client_name);\n\t\tusr = 0;\n\t}\n"""
new_auth = """\tok = 0;\n\tfor(account = cfg.account; usr && account; account = account->next)\n\t{\n\t\tif(strcmp((char *)usr, account->usr) != 0)\n\t\t\t{ continue; }\n\n\t\tmatched_account = account;\n\t\t__md5_crypt(ESTR(account->pwd), \"$1$abcdefgh$\", (char *)passwdcrypt);\n\t\tif(strcmp((char *)pwd, (const char *)passwdcrypt) != 0)\n\t\t{\n\t\t\tcs_log(\"newcamd authentication rejected for user %s: invalid credentials (%s)\", usr, client_name);\n\t\t\tbreak;\n\t\t}\n\n\t\tcl->crypted = 1;\n\t\tchar e_txt[20];\n\t\tsnprintf(e_txt, 20, \"%s:%d\", \"newcamd\", cfg.ncd_ptab.ports[cl->port_idx].s_port);\n\t\trc = cs_auth_client(cl, account, e_txt);\n\t\tif(rc == 2)\n\t\t{\n\t\t\tcs_log(\"newcamd authentication rejected for user %s: hostname or ip mismatch (%s)\", usr, client_name);\n\t\t\tbreak;\n\t\t}\n\t\tif(rc != 0)\n\t\t{\n\t\t\tcs_log(\"newcamd authentication rejected for user %s: account policy (%s)\", usr, client_name);\n\t\t\tbreak;\n\t\t}\n\n\t\tcs_log(\"user %s authenticated successfully (%s)\", usr, client_name);\n\t\tok = 1;\n\t\tbreak;\n\t}\n\n\tif(!matched_account)\n\t{\n\t\tcs_log(\"newcamd authentication rejected: unknown user %s (%s)\", usr, client_name);\n\t}\n\tif(!ok)\n\t\t{ cl->crypted = 0; }\n"""
s = rep(s, old_auth, new_auth, "deterministic account authentication")
p.write_text(s)


# Web-managed OSCam configuration: reject characters OSCam/our INI reader can
# interpret as comments.  Previously such a password could be saved, then be
# truncated when the page was reloaded, causing a silent authentication failure.
p = Path("src/OscamMiniManager.cpp")
s = p.read_text()
s = rep(
    s,
    "        if (user.password.empty() || user.password.size() > 64 || user.password.find('\\n') != std::string::npos || user.password.find('\\r') != std::string::npos) {\n",
    "        if (user.password.empty() || user.password.size() > 64 || user.password.find_first_of(\"\\r\\n;#\") != std::string::npos) {\n",
    "downstream password validation",
)
s = rep(
    s,
    '                reader.remotePassword.empty() || reader.remotePassword.size() > 256 ||\n                reader.remotePassword.find_first_of("\\r\\n") != std::string::npos) {\n',
    '                reader.remotePassword.empty() || reader.remotePassword.size() > 256 ||\n                reader.remotePassword.find_first_of("\\r\\n;#") != std::string::npos) {\n',
    "remote password validation",
)
s = rep(
    s,
    '            error = "Поддерживаются mouse/phoenix/pcsc: " + reader.label;\n',
    '            error = "Поддерживаются mouse/phoenix/pcsc/newcamd: " + reader.label;\n',
    "reader protocol validation message",
)
p.write_text(s)


# Document the authorization behavior and credential restriction.
p = Path("OSCAM_MINI.md")
d = p.read_text()
marker = "## Remote Newcamd readers\n"
auth_doc = """## Newcamd authentication hardening\n\nOSCam-mini validates the complete Newcamd login payload before reading the username\nor password hash. Unknown users, invalid credentials and account-policy rejection are\nhandled separately and password hashes are not written to the debug log. Passwords\nentered through the DVBStreamer5 OSCam-mini page must not contain `#` or `;`, because\nthose characters are OSCam/INI comment delimiters and cannot be round-tripped safely.\nThe Newcamd MD5-crypt wire format and the fixed `$1$abcdefgh$` salt remain protocol-\ncompatible with standard OSCam/Newcamd clients.\n\n"""
if auth_doc not in d:
    if marker not in d:
        raise SystemExit("documentation marker not found")
    d = d.replace(marker, auth_doc + marker, 1)
p.write_text(d)


# Version bump.
p = Path("src/AppVersion.h")
v = p.read_text()
v = rep(v, 'kProgramVersion = "10.8.62"', 'kProgramVersion = "10.8.63"', "version")
p.write_text(v)


# Guard against partial edits.
c = Path("third_party/oscam-mini/module-newcamd.c").read_text()
m = Path("src/OscamMiniManager.cpp").read_text()
assert "memchr(usr" in c and "memchr(pwd" in c
assert "matched_account" in c
assert "account->pwd=%s" not in c
assert "invalid credentials" in c
assert 'find_first_of("\\r\\n;#")' in m
assert 'kProgramVersion = "10.8.63"' in Path("src/AppVersion.h").read_text()
