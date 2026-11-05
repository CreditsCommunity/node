"""Run the set-up on a pseudo-terminal, as a user typing at it (Linux, macOS).

    pty_drive.py <folder> <command> 'REGEX=>ANSWER' ...

Each time the output ends with a prompt that matches a REGEX (searched in the output since the
last answer), ANSWER and Enter are typed. The wallet's own prompts are answered too: its passwords
(PTY_PASSWORD) and the words of a new recovery phrase it asks back ("word N: ", read from the phrase
it printed). Prints everything the command wrote and exits with its status; gives up after
PTY_TIMEOUT seconds (default 300).
"""
import os
import pty
import re
import select
import sys
import time

folder, command, specs = sys.argv[1], sys.argv[2], sys.argv[3:]
rules = [tuple(s.split("=>", 1)) for s in specs]
password = os.environ.get("PTY_PASSWORD", "a ci password")
rules += [(r"Password for the key file: $", password), (r"Repeat the password: $", password),
          (r"(?i)password: $", password)]

pid, fd = pty.fork()
if pid == 0:
    os.chdir(folder)
    os.execvp(command, [command])
out, buf, end = [], "", time.time() + int(os.environ.get("PTY_TIMEOUT", "300"))
while time.time() < end:
    ready, _, _ = select.select([fd], [], [], 1)
    if not ready:
        continue
    try:
        data = os.read(fd, 4096).decode(errors="replace")
    except OSError:
        break
    if not data:
        break
    out.append(data)
    buf += data
    m = re.search(r"word (\d+): $", buf)
    if m:
        words = dict(re.findall(r"(\d+)\. ([a-z]+)", "".join(out)))
        os.write(fd, (words[m.group(1)] + "\n").encode())
        buf = ""
        continue
    for pattern, answer in rules:
        if re.search(pattern + r"\s*$", buf):
            os.write(fd, (answer + "\n").encode())
            buf = ""
            break
sys.stdout.write("".join(out))
_, status = os.waitpid(pid, 0)
sys.exit(os.waitstatus_to_exitcode(status))
