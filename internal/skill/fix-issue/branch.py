#!/usr/bin/env python3
"""Mechanical checks for fix-issue worktree branches (see SKILL.md, Steps 3-4).

usage: branch.py status [WORKTREE]              what the branch really holds: commits ahead of
                                                master, porcelain status, diff stat, and any live
                                                process (build, compiler, agent) inside WORKTREE
       branch.py check-msg MSGFILE [--repo DIR] every internal/issue/ path named in a commit
                                                message is tracked, or deleted by the staged change
       branch.py squash WORKTREE -F MSGFILE     rebase onto master, prove master is an ancestor,
                                                squash to ONE commit with MSGFILE, print --stat

Each check exits 1 with the reason on the first violation, so `branch.py squash ... && next`
cannot run `next` on a failed rebase or a squash that would revert master's newer commits.
"""
import argparse, os, re, subprocess, sys

BASE = "master"
ISSUE_PATH = re.compile(r"internal/issue/[A-Za-z0-9_./-]+\.md")


def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit("git %s failed:\n%s%s" % (" ".join(args), r.stderr, r.stdout))
    return r.stdout.rstrip()


def fail(msg):
    sys.exit("branch.py: " + msg)


def run_lines(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()
    except FileNotFoundError:
        return None


def live_processes(worktree):
    """(pid, command line) of every process working in WORKTREE.

    Matches the command line, plus the executable path on Windows (an x64/<Config>/cflat.exe
    run) and the current directory on POSIX (a `ninja` started with a relative path). A Windows
    build tool started by relative path inside the worktree is not visible this way.
    """
    root = os.path.normcase(os.path.abspath(worktree))
    inside = lambda s: bool(s) and root in os.path.normcase(s.replace("/", os.sep))
    own = {str(os.getpid()), str(os.getppid())}
    hits = {}
    if os.name == "nt":
        lines = run_lines(["pwsh.exe", "-NoProfile", "-Command",
                           "Get-CimInstance Win32_Process | ForEach-Object "
                           "{ \"$($_.ProcessId)`t$($_.ExecutablePath)`t$($_.CommandLine)\" }"])
        if lines is None:
            return None
        for line in lines:
            pid, exe, args = (line.split("\t", 2) + ["", ""])[:3]
            if inside(exe) or inside(args):
                hits[pid] = args or exe
    else:
        lines = run_lines(["ps", "-eo", "pid=,args="])
        if lines is None:
            return None
        args_of = {}
        for line in lines:
            pid, _, args = line.strip().partition(" ")
            args_of[pid] = args.strip()
            if inside(args_of[pid]):
                hits[pid] = args_of[pid]
        pid = None
        for line in run_lines(["lsof", "-a", "-d", "cwd", "-F", "pn"]) or []:
            if line.startswith("p"):
                pid = line[1:]
            elif line.startswith("n") and pid and inside(line[1:]):
                hits[pid] = args_of.get(pid, "(cwd in worktree)")
    return [(pid, args) for pid, args in sorted(hits.items()) if pid not in own]


def cmd_status(a):
    wt = a.worktree
    ahead = git(wt, "rev-list", "--count", BASE + "..HEAD")
    print("branch:   %s (%s commit(s) ahead of %s)" % (git(wt, "branch", "--show-current"), ahead, BASE))
    log = git(wt, "log", "--oneline", BASE + "..HEAD")
    print(log if log else "(no commits)")
    porcelain = git(wt, "status", "--porcelain")
    print("\nworking tree: " + ("clean" if not porcelain else "\n" + porcelain))
    stat = git(wt, "diff", "--stat", BASE + "...HEAD")
    print("\ndiff vs merge-base:\n" + (stat if stat else "(empty)"))
    procs = live_processes(wt)
    if procs is None:
        print("\nlive processes: unknown (no process lister)")
    elif procs:
        print("\nLIVE PROCESSES in this worktree - it is occupied, do not escalate into it:")
        for pid, args in procs:
            print("  %s  %s" % (pid, args[:160]))
    else:
        print("\nlive processes: none")


def untracked_issue_paths(repo, message, deleted):
    """Issue paths named in MESSAGE that are neither tracked nor in DELETED (removed by the change)."""
    bad = []
    for path in sorted(set(ISSUE_PATH.findall(message))):
        tracked = subprocess.run(["git", "-C", repo, "ls-files", "--error-unmatch", path],
                                 capture_output=True).returncode == 0
        if not tracked and path not in deleted:
            bad.append(path)
    return bad


def read_message(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def cmd_check_msg(a):
    deleted = set(git(a.repo, "diff", "--cached", "--name-only", "--diff-filter=D").splitlines())
    bad = untracked_issue_paths(a.repo, read_message(a.msgfile), deleted)
    if bad:
        fail("commit message names issue files that are not in git (git add them in this "
             "commit, or drop the reference):\n  " + "\n  ".join(bad))
    print("check-msg: every issue path in the message is tracked or deleted by the change")


def cmd_squash(a):
    wt = a.worktree
    message = read_message(a.msgfile)
    if git(wt, "status", "--porcelain", "--untracked-files=no"):
        fail("worktree has uncommitted tracked changes; commit or discard them first")
    if git(wt, "rev-list", "--count", BASE + "..HEAD") == "0":
        fail("branch has no commits ahead of " + BASE)

    rebase = subprocess.run(["git", "-C", wt, "rebase", BASE], capture_output=True, text=True)
    if rebase.returncode != 0:
        fail("rebase onto %s failed - resolve or `git rebase --abort`, then rerun:\n%s%s"
             % (BASE, rebase.stdout, rebase.stderr))
    if git(wt, "diff", "--name-only", "--diff-filter=U"):
        fail("unmerged paths remain after the rebase")
    if subprocess.run(["git", "-C", wt, "merge-base", "--is-ancestor", BASE, "HEAD"]).returncode != 0:
        fail("%s is not an ancestor of HEAD after the rebase; a soft reset would revert it" % BASE)

    deleted = set(git(wt, "diff", "--name-only", "--diff-filter=D", BASE, "HEAD").splitlines())
    bad = untracked_issue_paths(wt, message, deleted)
    if bad:
        fail("commit message names issue files that are not in git (commit them on the branch, "
             "or drop the reference):\n  " + "\n  ".join(bad))

    # After the rebase BASE is the merge-base, so the soft reset keeps exactly the branch's change.
    tip = git(wt, "rev-parse", "HEAD")
    git(wt, "reset", "--soft", BASE)
    commit = subprocess.run(["git", "-C", wt, "commit", "-q", "-F", "-"], input=message,
                            capture_output=True, text=True)
    if commit.returncode != 0:
        git(wt, "reset", "--soft", tip)
        fail("commit failed; branch restored to %s:\n%s%s" % (tip[:10], commit.stdout, commit.stderr))
    print(git(wt, "log", "--oneline", "-1"))
    print(git(wt, "diff", "--stat", BASE + "..HEAD"))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status")
    s.add_argument("worktree", nargs="?", default=".")
    s.set_defaults(fn=cmd_status)
    m = sub.add_parser("check-msg")
    m.add_argument("msgfile")
    m.add_argument("--repo", default=".")
    m.set_defaults(fn=cmd_check_msg)
    q = sub.add_parser("squash")
    q.add_argument("worktree")
    q.add_argument("-F", dest="msgfile", required=True)
    q.set_defaults(fn=cmd_squash)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
