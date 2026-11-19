#!/usr/bin/env python3
"""Repository hygiene and release consistency checks (run in CI and before releases).

Checks every tracked file for:
  * secrets (API keys, tokens, private keys) and personal paths (C:/Users/<name>, /home/<name>);
  * files larger than 10 MB and stray control characters in text files;
  * version consistency: CMakeLists.txt, installer default, AppStream metainfo, optional git tag;
  * GitHub Actions pinned to full commit SHAs, workflow permissions declared.
Exit code 0 = clean.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = (".png", ".ico", ".bmp", ".pdf", ".dll", ".lib", ".traineddata", ".so", ".tgz", ".zip")
SECRET_PATTERNS = [
    (r"gh[pousr]_[A-Za-z0-9]{30,}", "GitHub token"),
    (r"AKIA[0-9A-Z]{16}", "AWS access key"),
    (r"-----BEGIN (RSA |OPENSSH |EC |DSA |)PRIVATE KEY-----", "private key"),
    (r"sk-[A-Za-z0-9]{32,}", "API key"),
    (r"xox[baprs]-[A-Za-z0-9-]{10,}", "Slack token"),
    (r"(?i)(password|passwd|secret|api_key|apikey|token)\s*[:=]\s*['\"][^'\"\s]{8,}['\"]", "hard-coded credential"),
]
PERSONAL_PATHS = [r"[A-Za-z]:[\\/]+Users[\\/]+(?!Public|Default)[A-Za-z0-9._-]+[\\/]", r"/home/(?!runner|ubuntu|tester|user|mintuser)[a-z0-9._-]+/"]
ALLOW_PERSONAL = {"tools/check_repo.py"}


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    return [f for f in out.decode("utf-8").split("\0") if f]


def main():
    problems = []
    for rel in tracked_files():
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue
        size = os.path.getsize(path)
        if size > 10 * 1024 * 1024:
            problems.append(f"{rel}: {size // (1024 * 1024)} MB tracked file (limit 10 MB)")
        if rel.lower().endswith(BINARY):
            continue
        with open(path, "rb") as fh:
            data = fh.read()
        if re.search(rb"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", data):
            problems.append(f"{rel}: contains control characters")
        text = data.decode("utf-8", errors="replace")
        if rel.startswith("third_party/"):
            continue
        for pattern, what in SECRET_PATTERNS:
            for m in re.finditer(pattern, text):
                line = text.count("\n", 0, m.start()) + 1
                problems.append(f"{rel}:{line}: possible {what}")
        if rel not in ALLOW_PERSONAL:
            for pattern in PERSONAL_PATHS:
                for m in re.finditer(pattern, text):
                    line = text.count("\n", 0, m.start()) + 1
                    problems.append(f"{rel}:{line}: personal path {m.group(0)!r}")

    def read(rel):
        with open(os.path.join(ROOT, rel), encoding="utf-8") as fh:
            return fh.read()

    version = re.search(r"project\(Folvio\s+VERSION\s+([0-9.]+)", read("CMakeLists.txt")).group(1)
    iss = re.search(r'#define AppVersion "([0-9.]+)"', read("packaging/windows/folvio.iss")).group(1)
    meta = re.search(r'<release version="([0-9.]+)"', read("packaging/linux/io.github.twivo1.Folvio.metainfo.xml")).group(1)
    for name, v in (("installer default", iss), ("AppStream metainfo", meta)):
        if v != version:
            problems.append(f"version mismatch: CMakeLists.txt {version} vs {name} {v}")
    tag = os.environ.get("RELEASE_TAG", "")
    if tag:
        base = tag[1:] if tag.startswith("v") else tag
        if base.split("-")[0] != version:
            problems.append(f"tag {tag} does not match CMakeLists.txt version {version}")

    wf_dir = os.path.join(ROOT, ".github", "workflows")
    if os.path.isdir(wf_dir):
        for name in sorted(os.listdir(wf_dir)):
            text = read(f".github/workflows/{name}")
            for m in re.finditer(r"uses:\s*([^\s#]+)", text):
                ref = m.group(1)
                if ref.startswith("./"):
                    continue
                if not re.search(r"@[0-9a-f]{40}$", ref):
                    problems.append(f".github/workflows/{name}: action not pinned to a commit SHA: {ref}")
            if not re.search(r"^permissions:", text, re.M):
                problems.append(f".github/workflows/{name}: no top-level permissions block")

    if problems:
        print("Repository check FAILED:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"Repository check OK (version {version}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
