from pathlib import Path

p = Path("scripts/apply_v10862_remote_newcamd_ui.py")
s = p.read_text()
old = '''s = rep(
    s,
    '                || log.find(reader.label + " [pcsc] found card system") != std::string::npos;\\n',
    '                || log.find(reader.label + " [pcsc] found card system") != std::string::npos\\n                || log.find(reader.label + " [newcamd] connecting to ") != std::string::npos;\\n',
    "remote status connecting",
)
'''
new = '''s = rep(
    s,
    '                || log.find(reader.label + " [pcsc] found card system") != std::string::npos\\n                || log.find(reader.label + " [irdeto] THIS WAS A SUCCESSFUL START ATTEMPT") != std::string::npos;\\n',
    '                || log.find(reader.label + " [pcsc] found card system") != std::string::npos\\n                || log.find(reader.label + " [irdeto] THIS WAS A SUCCESSFUL START ATTEMPT") != std::string::npos\\n                || log.find(reader.label + " [newcamd] connecting to ") != std::string::npos;\\n',
    "remote status connecting",
)
'''
if s.count(old) != 1:
    raise SystemExit(f"status patch expected one block, got {s.count(old)}")
p.write_text(s.replace(old, new, 1))
