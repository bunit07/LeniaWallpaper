#!/usr/bin/env python3
"""Archive Lenia species parameter sets from evolvecode.io/alife/lenia.

Source: https://evolvecode.io/alife/lenia.html
(Emergent Garden — Artificial Life demo)

Downloads the named list (lenia_saves/_list.json) and the discovered list
(lenia_saves/discovered/_list.json), then every species file referenced by them,
into catalog/named/ and catalog/discovered/ (raw, unmodified). Finally writes
catalog/index.json — a flat list of {path, name} entries the wallpaper app uses
for random selection.

Resumable: files already on disk that parse as JSON are skipped.
"""

import json
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

BASE = "https://evolvecode.io/alife/lenia_saves/"
ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "catalog"
WORKERS = 12
RETRIES = 4
UA = {"User-Agent": "lenia-wallpaper-catalog-fetch/1.0 (one-time archival)"}


def fetch(url: str) -> bytes:
    last_err = None
    for attempt in range(RETRIES):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.read()
        except Exception as e:  # noqa: BLE001 - retry on any transport error
            last_err = e
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"failed after {RETRIES} attempts: {url}: {last_err}")


def valid_json_on_disk(path: Path) -> bool:
    if not path.is_file() or path.stat().st_size == 0:
        return False
    try:
        json.loads(path.read_text(encoding="utf-8"))
        return True
    except (json.JSONDecodeError, UnicodeDecodeError, OSError):
        return False


def download_one(url: str, dest: Path) -> str:
    if valid_json_on_disk(dest):
        return "skipped"
    data = fetch(url)
    json.loads(data)  # validate before committing to disk
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return "downloaded"


def main() -> int:
    CATALOG.mkdir(exist_ok=True)

    named_list = json.loads(fetch(BASE + "_list.json"))
    disc_list = json.loads(fetch(BASE + "discovered/_list.json"))
    (CATALOG / "named").mkdir(exist_ok=True)
    (CATALOG / "discovered").mkdir(exist_ok=True)
    (CATALOG / "named" / "_list.json").write_text(
        json.dumps(named_list, indent=1), encoding="utf-8")
    (CATALOG / "discovered" / "_list.json").write_text(
        json.dumps(disc_list, indent=1), encoding="utf-8")

    jobs = []  # (url, dest, index_path, display_name)
    for e in named_list["states"]:
        jobs.append((BASE + e["file"], CATALOG / "named" / e["file"],
                     "named/" + e["file"], e["name"]))
    for e in disc_list["states"]:
        jobs.append((BASE + "discovered/" + e["file"],
                     CATALOG / "discovered" / e["file"],
                     "discovered/" + e["file"], e["name"]))

    print(f"{len(jobs)} species files "
          f"({len(named_list['states'])} named, {len(disc_list['states'])} discovered)")

    done = skipped = 0
    failures = []
    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        futs = {pool.submit(download_one, url, dest): (url, dest)
                for url, dest, _, _ in jobs}
        for fut in as_completed(futs):
            url, _ = futs[fut]
            try:
                if fut.result() == "skipped":
                    skipped += 1
                else:
                    done += 1
            except Exception as e:  # noqa: BLE001
                failures.append((url, str(e)))
            n = done + skipped + len(failures)
            if n % 500 == 0:
                print(f"  {n}/{len(jobs)}  (new {done}, cached {skipped}, failed {len(failures)})",
                      flush=True)

    print(f"finished: new {done}, cached {skipped}, failed {len(failures)}")
    for url, err in failures[:20]:
        print(f"  FAIL {url}: {err}")
    if failures:
        print("re-run this script to retry the failures")
        return 1

    index = [{"path": rel, "name": name} for _, _, rel, name in jobs]
    (CATALOG / "index.json").write_text(
        json.dumps({"count": len(index), "species": index}, indent=1),
        encoding="utf-8")
    print(f"wrote catalog/index.json with {len(index)} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
