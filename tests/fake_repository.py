#!/usr/bin/env python3
"""A tiny fake Hydrus tag repository for integration tests.

Serves /metadata and /update with the same zlib-compressed serialisable JSON
format the real PTR uses. Test data is intentionally small and deterministic.
"""

import argparse
import hashlib
import json
import threading
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ACCESS_KEY = "11" * 32


def pack(value):
    return zlib.compress(json.dumps(value).encode())


def serialise(obj_type, info):
    return [obj_type, 1, info]


def definitions(hashes, tags):
    groups = []
    if hashes:
        groups.append([0, [[i, h] for i, h in hashes]])
    if tags:
        groups.append([1, [[i, t] for i, t in tags]])
    return serialise(36, groups)


def content(mapping_add=None, mapping_delete=None, file_delete=None, siblings=None, parents=None):
    groups = []
    if mapping_add or mapping_delete:
        actions = []
        if mapping_add:
            actions.append([0, [[tag_id, hash_ids] for tag_id, hash_ids in mapping_add]])
        if mapping_delete:
            actions.append([1, [[tag_id, hash_ids] for tag_id, hash_ids in mapping_delete]])
        groups.append([0, actions])
    if siblings:
        actions = []
        for action, pairs in siblings:
            actions.append([action, [[a, b] for a, b in pairs]])
        groups.append([1, actions])
    if parents:
        actions = []
        for action, pairs in parents:
            actions.append([action, [[a, b] for a, b in pairs]])
        groups.append([2, actions])
    if file_delete:
        groups.append([3, [[1, file_delete]]])
    return serialise(34, groups)


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


class Repository:
    def __init__(self):
        self.updates = []  # (index, bytes, timestamp)
        self.index_to_hashes = {}
        self.next_index = 0

    def add_index(self, update_bytes):
        index = self.next_index
        self.next_index += 1
        digest = hashlib.sha256(update_bytes).hexdigest()
        self.updates.append((index, digest, update_bytes, 1700000000 + index))
        self.index_to_hashes.setdefault(index, []).append(digest)
        return digest


def build_repository(mode):
    repo = Repository()

    h1 = sha("alpha-file")
    h2 = sha("beta-file")
    h3 = sha("gamma-file")
    legacy = hashlib.sha1(b"legacy-file").hexdigest()

    # index 0: definitions for two files and three tags. The legacy SHA-1
    # hash mirrors the rare non-SHA-256 definitions the real PTR contains.
    repo.add_index(pack(definitions(
        [(10, h1), (11, h2), (13, legacy)],
        [(20, "series:alpha"), (21, "character:beta"), (22, "creator:gamma")],
    )))

    # index 1: mappings, a sibling (20 -> 21) and a parent (21 -> 22).
    repo.add_index(pack(content(
        mapping_add=[(20, [10, 11, 13]), (21, [10]), (22, [11])],
        siblings=[(0, [(20, 21)])],
        parents=[(0, [(21, 22)])],
    )))

    if mode == "incremental":
        # index 2: one new file, one new legacy SHA-1 file and one new tag.
        legacy_two = hashlib.sha1(b"legacy-two").hexdigest()
        repo.add_index(pack(definitions([(12, h3), (14, legacy_two)], [(23, "character:delta")])))
        # index 3: add a mapping, delete a mapping, add a parent.
        repo.add_index(pack(content(
            mapping_add=[(23, [12]), (22, [12]), (22, [14])],
            mapping_delete=[(20, [11])],
            parents=[(0, [(23, 22)])],
        )))

    if mode == "published":
        # The same four indices as "incremental", plus two freshly published
        # ones. Used to prove that a resumed initial build finishes its old
        # snapshot before catching up on new indices.
        legacy_two = hashlib.sha1(b"legacy-two").hexdigest()
        repo.add_index(pack(definitions([(12, h3), (14, legacy_two)], [(23, "character:delta")])))
        repo.add_index(pack(content(
            mapping_add=[(23, [12]), (22, [12]), (22, [14])],
            mapping_delete=[(20, [11])],
            parents=[(0, [(23, 22)])],
        )))
        h4 = sha("epsilon-file")
        repo.add_index(pack(definitions([(15, h4)], [(24, "series:epsilon")])))
        repo.add_index(pack(content(mapping_add=[(24, [15])])))

    if mode == "future":
        # index 2: a content type this build does not understand. hydd must
        # refuse to sync instead of silently dropping it.
        repo.add_index(pack(serialise(34, [[9, [[0, []]]]])))

    if mode == "badhash":
        # index 2: an invalid hash definition that is not one of the four
        # known PTR values. hydd must refuse to sync.
        repo.add_index(pack(definitions([(15, "ffff")], [(24, "bad:tag")])))

    if mode == "knownbad":
        # index 2: one of the four known-invalid PTR values. hydd skips it.
        repo.add_index(pack(definitions([(15, "0000")], [(24, "bad:tag")])))

    return repo, {"h1": h1, "h2": h2, "h3": h3}


class Handler(BaseHTTPRequestHandler):
    repo = None
    metadata_bytes = None
    corrupt_indices = set()
    missing_indices = set()

    def log_message(self, fmt, *args):
        pass

    def _send(self, body, status=200):
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.headers.get("Hydrus-Key") != ACCESS_KEY:
            self.send_error(401)
            return

        parsed = urlparse(self.path)
        if parsed.path == "/metadata":
            since = int(parse_qs(parsed.query).get("since", ["0"])[0])
            rows = []
            for index in sorted(self.repo.index_to_hashes):
                if index < since:
                    continue
                rows.append([index, self.repo.index_to_hashes[index], 1700000000 + index, 1700000100 + index])
            payload = [21, 2, [[[0, "metadata_slice"], [2, [37, 1, [rows, 1800000000]]]]]]
            self._send(pack(payload))
            return

        if parsed.path == "/update":
            digest = parse_qs(parsed.query).get("update_hash", [""])[0]
            for index, hashes in self.repo.index_to_hashes.items():
                if digest in hashes:
                    if index in self.missing_indices:
                        self.send_error(404)
                        return
                    if index in self.corrupt_indices:
                        self._send(b"this is not the update you are looking for")
                        return
            for _, update_hash, body, _ in self.repo.updates:
                if update_hash == digest:
                    self._send(body)
                    return
            self.send_error(404)
            return

        self.send_error(404)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=45871)
    parser.add_argument("--mode", choices=["initial", "incremental", "published", "future", "badhash", "knownbad"], default="initial")
    parser.add_argument("--corrupt-index", type=int, action="append", default=[],
                        help="serve a body that does not match its hash for this update index")
    parser.add_argument("--missing-index", type=int, action="append", default=[],
                        help="return 404 for this update index")
    args = parser.parse_args()

    repo, hashes = build_repository(args.mode)
    Handler.repo = repo
    Handler.corrupt_indices = set(args.corrupt_index)
    Handler.missing_indices = set(args.missing_index)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(json.dumps({"port": args.port, "hashes": hashes, "updates": len(repo.updates)}), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
