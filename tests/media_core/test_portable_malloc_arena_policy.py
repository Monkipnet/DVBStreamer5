#!/usr/bin/env python3
"""V492: test exact allocator export shipped by portable run.sh generator."""
import os
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "scripts" / "make_portable_linux_bundle.sh").read_text()
start = source.index('cat > "$STAGE/run.sh" <<EOF_RUN')
end = source.index('\nEOF_RUN', start)
launcher = source[start:end]
exports = [line for line in launcher.splitlines()
           if line.startswith("export MALLOC_ARENA_MAX=")]
assert len(exports) == 1, "exactly one allocator setting is expected"
line = exports[0]
assert "MALLOC_ARENA_MAX:-2" in line, "portable default must be 2"

# Unescape shell heredoc variable quoting to get the actual shipped launcher.
effective = line.replace('\\' + '$' + '{', '$' + '{')
assert effective == 'export MALLOC_ARENA_MAX="' + '$' + '{MALLOC_ARENA_MAX:-2}"'

def check(value):
    env = os.environ.copy()
    env.pop("MALLOC_ARENA_MAX", None)
    if value is not None:
        env["MALLOC_ARENA_MAX"] = str(value)
    command = effective + '; printf "%s" "$MALLOC_ARENA_MAX"'
    done = subprocess.run(["bash", "-c", command], env=env,
                          check=True, stdout=subprocess.PIPE, text=True)
    return done.stdout

assert check(None) == "2", "default value must be 2"
assert check("4") == "4", "service override must be preserved"
assert check("8") == "8", "manual override must be preserved"
print("PASS: portable malloc arenas default=2, explicit overrides preserved")
