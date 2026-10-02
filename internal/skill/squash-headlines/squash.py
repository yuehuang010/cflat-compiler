#!/usr/bin/env python3
"""Squash origin/master..master into topical headline commits.

usage: squash.py PLAN.json            dry run: build the grouped chain, verify, print a summary
       squash.py PLAN.json --apply    dry run, then back up master and move it to the new chain

PLAN.json: {"groups": [{"title": "...", "commits": ["abcd1234", ...]}, ...,
                       {"title": "...", "commits": [...], "bookkeeping": true}]}
Every commit in origin/master..master must appear in exactly one group. A group's commits
replay in their original order. Groups are emitted greedily in plan order; a group whose
commits cannot all move ahead of another group's (textual conflict) is split into
"(1/n)" parts at the conflict point, so put the most independent topics first. At most one bookkeeping group, and it must be last: old
hashes in the tracked files it ships are rewritten to the grouped commits' hashes.
Commits are built with merge-tree / commit-tree, so the work tree is untouched until --apply.
"""
import itertools, json, re, subprocess, sys, datetime, os, tempfile

TRAILER = "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"


def git(*a, inp=None, check=True, env=None):
    r = subprocess.run(["git", *a], capture_output=True, text=True, input=inp, env=env)
    if check and r.returncode != 0:
        sys.exit("git %s failed:\n%s%s" % (" ".join(a), r.stderr, r.stdout))
    return r


def out(*a, **k):
    return git(*a, **k).stdout.strip()


MEMO = {}
ORDER = {}


def pick(base, c):
    """Replay commit c onto base; return the new commit or None on conflict (memoized)."""
    if (base, c) not in MEMO:
        MEMO[(base, c)] = pick_uncached(base, c)
    return MEMO[(base, c)]


def patch_id(a, b):
    d = git("diff", "-U0", "--binary", "--full-index", a, b).stdout
    return out("patch-id", "--stable", inp=d) if d else ""


def pick_uncached(base, c):
    r = git("merge-tree", "--write-tree", "--merge-base=" + c + "^", base, c, check=False)
    if r.returncode != 0:
        return None
    new = out("commit-tree", r.stdout.split()[0], "-p", base, "-m", "tmp " + c)
    # A clean merge can still drop part of c (deleting a file base never had merges as
    # "both deleted"), so the replay must reproduce c's exact change or it is a conflict.
    if patch_id(base, new).split()[:1] != patch_id(c + "^", c).split()[:1]:
        return None
    return new


def files(c):
    return set(out("diff-tree", "--no-commit-id", "--name-only", "-r", c + "^", c).splitlines())


def blockers(c, groups, rest, later=False):
    """Commits in other groups that touch the files c touches: earlier ones still pending, or
    (later=True) later ones already replayed ahead of c."""
    mine = files(c)
    hits = []
    for g in groups:
        if c in rest[id(g)]:
            continue
        for o in rest[id(g)]:
            if (ORDER[o] > ORDER[c]) == later:
                common = mine & files(o)
                if common:
                    hits.append("  %s [%s] %s" % (o[:8], g["title"][:40], ", ".join(sorted(common)[:3])))
    return "\n".join(hits) or "  (none found: c conflicts with already emitted content)"


def schedule(base, groups):
    """Greedy: repeatedly emit the first group (plan order) whose remaining commits all replay
    cleanly; if none can, emit the longest clean prefix of any group. A group emitted in several
    pieces is titled "(1/n)"... Returns [(group, commits, tip)] or exits on a dead end."""
    body = [g for g in groups if not g.get("bookkeeping")]
    rest = {id(g): list(g["commits"]) for g in groups}
    chunks = []
    def prefix(b, cs):
        n = 0
        for c in cs:
            nb = pick(b, c)
            if nb is None:
                break
            b, n = nb, n + 1
        return n, b
    while any(rest[id(g)] for g in body):
        best = None
        for g in body:
            cs = rest[id(g)]
            if not cs:
                continue
            n, b = prefix(base, cs)
            if n == len(cs):
                best = (g, n, b)
                break
            if n and (best is None or n > best[1]):
                best = (g, n, b)
        if best is None:
            first = min((c for g in body for c in rest[id(g)]), key=ORDER.get)
            sys.exit("dead end: %s conflicts on every schedule; regroup it. It depends on:\n%s"
                     % (first[:8], blockers(first, groups, rest)))
        g, n, base = best
        chunks.append((g, rest[id(g)][:n], base))
        rest[id(g)] = rest[id(g)][n:]
    for g in groups:
        if g.get("bookkeeping"):
            n, b = prefix(base, g["commits"])
            if n != len(g["commits"]):
                c = g["commits"][n]
                sys.exit("bookkeeping commit %s conflicts at the end of the chain; later commits replayed ahead of it:\n%s"
                         % (c[:8], blockers(c, groups, {id(x): x["commits"] for x in groups}, True)))
            chunks.append((g, g["commits"], b))
    total = {}
    for g, _, _ in chunks:
        total[id(g)] = total.get(id(g), 0) + 1
    seen, out_ = {}, []
    for g, cs, tip in chunks:
        seen[id(g)] = seen.get(id(g), 0) + 1
        t = g["title"] if total[id(g)] == 1 else "%s (%d/%d)" % (g["title"], seen[id(g)], total[id(g)])
        out_.append((dict(g, title=t), cs, tip))
    return out_


def remap_text(s, remap):
    """Rewrite any 7-40 hex abbreviation of an old hash to the same-length new hash."""
    def sub(m):
        w = m.group(0)
        hits = [o for o in remap if o.startswith(w)]
        return remap[hits[0]][:len(w)] if len(hits) == 1 else w
    return re.sub(r"\b[0-9a-f]{7,40}\b", sub, s)


def message(title, commits, remap, reword=None):
    if len(commits) == 1 and title == out("log", "-1", "--format=%s", commits[0]).strip():
        # an earlier headline kept as its own group: message verbatim
        return remap_text(out("log", "-1", "--format=%B", commits[0]).rstrip(), remap) + "\n"
    lines = [title, ""]
    for c in commits:
        body = next((v for k, v in (reword or {}).items() if c.startswith(k)), None) \
            or out("log", "-1", "--format=%B", c)
        body = " ".join(l.strip() for l in body.splitlines()
                        if l.strip() and not l.startswith("Co-Authored-By:"))
        lines.append("- %s (was %s)" % (remap_text(body, remap), c[:8]))
    return "\n".join(lines) + "\n\n" + TRAILER + "\n"


def remapped_tree(tree, remap):
    """Return (new tree, files changed) with old hashes rewritten in tracked text files."""
    pat = "|".join(o[:7] for o in remap)
    r = git("grep", "-l", "-I", "-E", pat, tree, check=False)
    files = [l.split(":", 1)[1] for l in r.stdout.splitlines() if l]
    if not files:
        return tree, []
    fd, idx = tempfile.mkstemp(prefix="squash_idx_")
    os.close(fd)
    env = dict(os.environ, GIT_INDEX_FILE=idx)
    git("read-tree", tree, env=env)
    changed = []
    for f in files:
        s = git("cat-file", "blob", "%s:%s" % (tree, f)).stdout
        s2 = remap_text(s, remap)
        if s2 != s:
            mode = out("ls-tree", tree, "--", f).split()[0]
            blob = out("hash-object", "-w", "--stdin", inp=s2)
            git("update-index", "--cacheinfo", "%s,%s,%s" % (mode, blob, f), env=env)
            changed.append(f)
    new = out("write-tree", env=env)
    os.unlink(idx)
    return new, changed


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    plan = json.load(open(sys.argv[1]))
    apply_ = "--apply" in sys.argv
    groups = plan["groups"]
    if out("rev-parse", "--abbrev-ref", "HEAD") != "master":
        sys.exit("not on master")
    if out("status", "--porcelain", "--untracked-files=no"):
        sys.exit("tracked changes present; commit or move them first")
    git("fetch", "-q", "origin")
    if git("merge-base", "--is-ancestor", "origin/master", "master", check=False).returncode:
        sys.exit("origin/master is not an ancestor of master: rebase onto origin/master first")
    todo = out("rev-list", "--reverse", "origin/master..master").split()
    order = ORDER
    order.update({c: i for i, c in enumerate(todo)})
    seen = {}
    for g in groups:
        full = []
        for c in g["commits"]:
            f = out("rev-parse", "--verify", c + "^{commit}")
            if f not in order:
                sys.exit("%s is not in origin/master..master" % c)
            if f in seen:
                sys.exit("%s is in two groups" % c)
            seen[f] = g["title"]
            full.append(f)
        g["commits"] = sorted(full, key=order.get)
    missing = [c[:8] for c in todo if c not in seen]
    if missing:
        sys.exit("commits not in any group: " + " ".join(missing))
    bk = [i for i, g in enumerate(groups) if g.get("bookkeeping")]
    if bk and bk != [len(groups) - 1]:
        sys.exit("only the last group may be bookkeeping")

    base = out("rev-parse", "origin/master")
    chunks = schedule(base, groups)
    master = out("rev-parse", "master")
    if out("rev-parse", chunks[-1][2] + "^{tree}") != out("rev-parse", master + "^{tree}"):
        sys.exit("tree mismatch against master - aborting")

    remap, parent = {}, base
    for g, cs, tip in chunks:
        tree = out("rev-parse", tip + "^{tree}")
        changed = []
        if g.get("bookkeeping"):
            tree, changed = remapped_tree(tree, remap)
        parent = out("commit-tree", tree, "-p", parent, inp=message(g["title"], cs, remap, plan.get("reword")))
        for c in cs:
            remap[c] = parent
        print("%s %2d commits  %s" % (parent[:8], len(cs), g["title"][:110]))
        for f in changed:
            print("           hashes remapped in " + f)
    d = out("diff", "--stat", master, parent)
    print("new tip %s; diff vs master: %s" % (parent[:8], d.splitlines()[-1] if d else "IDENTICAL"))
    json.dump({"tip": parent, "old": master, "remap": {k[:8]: v[:8] for k, v in remap.items()}},
              open(os.path.splitext(sys.argv[1])[0] + ".remap.json", "w"), indent=1)
    if not apply_:
        print("dry run; rerun with --apply to move master")
        return
    name = "backup/master-pre-squash-" + datetime.date.today().isoformat()
    n = 2
    while git("rev-parse", "--verify", "-q", name, check=False).returncode == 0:
        name = "backup/master-pre-squash-%s-%d" % (datetime.date.today().isoformat(), n)
        n += 1
    git("branch", name, master)
    git("reset", "--keep", parent)
    print("master -> %s (backup %s)" % (parent[:8], name))


if __name__ == "__main__":
    main()
