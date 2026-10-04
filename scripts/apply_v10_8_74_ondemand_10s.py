from pathlib import Path

p = Path('src/StreamManager.cpp')
s = p.read_text()
s2 = s.replace('constexpr auto kIdleGrace = std::chrono::seconds(5);',
               'constexpr auto kIdleGrace = std::chrono::seconds(10);')
s2 = s2.replace('reason=no-clients idle_s=5', 'reason=no-clients idle_s=10')
if s2 == s:
    raise SystemExit('StreamManager replacements not applied')
p.write_text(s2)

p = Path('src/AppVersion.h')
s = p.read_text()
s2 = s.replace('kProgramVersion = "10.8.73"', 'kProgramVersion = "10.8.74"')
if s2 == s:
    raise SystemExit('AppVersion replacement not applied')
p.write_text(s2)
