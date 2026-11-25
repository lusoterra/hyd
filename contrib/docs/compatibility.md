# Hydrus compatibility

Hydrus is three separate things that are easy to conflate:

1. **The PTR (Public Tag Repository)** - a Hydrus *server*. It stores
   hash-to-tag mappings and tag relationships. It serves an update stream over
   the repository protocol (`/metadata`, `/update`) and accepts uploads and
   petitions from accounts with write permission. It has no media and no
   general file-query API.
2. **The Hydrus client** - a local application with its own SQLite database. It
   downloads the PTR update stream, applies it locally, and adds everything the
   PTR never stores: files, thumbnails, URLs, notes, ratings, local tags,
   inbox/archive state, pages, and history.
3. **The Hydrus Client API** - a localhost HTTP API *served by the client* for
   automation of that client. Endpoints such as `/get_files/file`,
   `/get_files/thumbnail`, `/get_files/search_files`, `/add_files/add_file`,
   and `/add_tags/add_tags` operate on the client's local database and files.

The PTR does not implement the Client API. The client does not use a remote
Client API as its data source. "Download the whole repository" is how Hydrus
keeps queries local: each client builds its own tag database instead of asking
a central service what to tag.

## Where hyd fits

Hyd is a **read-only hybrid**:

| Role | Hyd support |
| --- | --- |
| PTR repository update stream, read side (`/metadata`, `/update`) | Yes, when `keep_update_cache = true` and the cache is complete |
| PTR uploads, petitions, accounts, janitor review | No |
| Hydrus Client API read-only lookups (`/get_files/file_metadata`, `/get_files/file_hashes`, access/session keys, services, client info) | Partial, read-only |
| Client API media, thumbnails, search, imports, tag edits, notes, ratings, pages | No, returns `501` JSON |
| hydd extensions (`/hyd/stats`, `/hyd/tag_counts`, `/hyd/tag_hashes`) | Yes |

Hyd is not a Hydrus client, not a writable PTR, and not a drop-in replacement
for the client's local database. It answers "which tags does the PTR have for
this hash?" and can relay the PTR stream to another mirror.

## What works today

- **hydd to hydd mirroring.** A second `hydd` can sync from a `hydd` relay
  exactly as it syncs from the PTR: `repository_url` points at the relay,
  `repository_access_key` matches a key the relay accepts, and the client sends
  `Hydrus-Key` on each `/metadata` and `/update` request. This is covered by the
  integration tests.
- **Read-only Client API consumers.** Tools that only need hash lookup and tag
  metadata can use `/get_files/file_metadata`, `/get_files/file_hashes`,
  `/hyd/tag_counts`, and `/hyd/tag_hashes`. File facts are `null`/empty, and
  unsupported endpoints return `501` with a reason.
- **Bulk local indexing.** `/hyd/tag_hashes` pages hashes for a tag query, so a
  client can build its own cache without downloading the whole stream.

## Why the official Hydrus client is not supported yet

The data model is not the problem. A Hydrus client always downloads the whole
repository and builds its tags locally, and Hyd can serve that whole stream when
`keep_update_cache = true` and the cache is complete. Once the client can add
and authenticate to the repository, it would sync the tag repository normally,
keep all of its local files, thumbnails, search, notes, ratings, pages, and
imports, and behave like a client with a read-only tag repository. Only PTR
writes - submitting hashes, tags, and petitions - would be unavailable.

The current gap is the repository protocol envelope around that stream, not the
stream itself.

A Hydrus client can only use a repository after its repository login and
account sync succeed. The client:

- calls `GET /service_info` when the repository is added, to learn its service
  key, type, and name;
- calls `GET /session_key` with `Hydrus-Key` and expects the server to set a
  `session_key` **cookie**; every later request carries that cookie;
- calls `GET /account` and `GET /options` to learn whether the account is
  functional and what it may do;
- expects HTTPS for repository URLs.

Hyd currently:

- answers `/session_key` with the **Client API** JSON response (a session key in
  the body, no cookie), because that path is shared with the Client API;
- does not accept cookies for authentication;
- does not implement `/service_info`, `/account`, `/options`, or
  `/access_key_verification`;
- serves plain HTTP and does not terminate TLS;
- has no write, petition, or account endpoints at all.

The metadata/update stream itself is correct and hyd-to-hyd sync passes, but the
official client's login/account checks will not complete against Hyd as it is
today. Do not advertise Hyd as a drop-in PTR replacement for the Hydrus client
yet. Treat official-client support as unimplemented rather than as a bug in the
relay.

## What it would take to support the official client

At minimum:

1. Implement `/service_info` so the client can add the repository as a tag
   repository with the configured service key and name.
2. Distinguish repository `/session_key` from Client API `/session_key`. The
   repository login sends `Hydrus-Key`; the Client API sends
   `Hydrus-Client-API-Access-Key` or `Hydrus-Client-API-Session-Key`. The
   repository path needs a `Set-Cookie: session_key=...` response.
3. Accept that cookie on `/metadata` and `/update` for non-loopback clients.
4. Implement a synthetic read-only account for `/account` and `/options`, plus
   `/access_key_verification`. The account must be functional but grant no
   upload or petition permission.
5. Put HTTPS in front, since the client uses `https://` for repositories.
6. Decide what to do about the client's periodic account/options polling and
   its "service info" display. A read-only account can work, but it will show
   no write permissions.

Worth asking the Hydrus developer before building this: whether a minimal
read-only repository server is an intended use case, which of the account and
service-info responses are mandatory for login, and whether they would accept a
documented compatibility profile. Hyd already serves the exact metadata and
update bytes, so the gap is the surrounding account/session protocol, not the
stream format.

## A read-only mirror profile to propose

The protocol is already split by method and path: `GET /metadata` and
`GET /update` read; `POST /update` and `/petition` write. The ask is not a new
API, it is a declared read-only mode that clients can accept without pretending
the mirror is a full PTR:

- document a minimal read-only repository profile: `/service_info`,
  `/session_key` with a session cookie, `/metadata`, and `GET /update`;
- have `/service_info` relay the upstream repository identity (same service
  key, type, and name) plus a read-only flag, so the mirror is the same service
  with the same tags rather than a second, unrelated repository;
- return a functional account from `/account` and `/options` that has no
  upload or petition permission, or let the client skip account sync for a
  service marked read-only;
- make the client show such a service as read-only, without repeated upload
  attempts or account-error popups;
- require mirrors to relay bytes unmodified and keep TLS; writes always go to
  the authoritative PTR, never through a mirror.

This fits the Hydrus model rather than fighting it. Clients still download
everything and keep their own local database, so there is no central query
oracle. Mirrors reduce load on the PTR, but writes stay centralized and
moderated. That does not make a mirror tamper-proof: repository metadata is not
signed, so a malicious mirror can serve a divergent stream just like any other
repository host. Relaying the exact bytes over TLS is the expected behaviour,
not a cryptographic guarantee.

The main identity question is the service key. Hyd currently generates its own
`service_key` and would need to fetch and relay the upstream repository's
service info for a transparent mirror; otherwise a client sees a separate tag
repository with the same tags rather than the same service.

## Do not proxy writes to the PTR

Hyd must not forward uploads or petitions to the official PTR. That would need
the user's PTR account credentials, would make every Hyd user share one
identity, would bypass the PTR's own account, rate-limit, janitor, and ban
model, and would quietly make the mirror a write path the owner did not agree
to operate. A read-only account with no petition permission is the supported
shape: the client syncs and queries, and PTR submissions remain a separate
action in the user's own client against the real PTR.

## If you host Hyd publicly

- It is a read-only mirror plus a hash/tag query oracle. Clients can download
  the update stream and query hashes; nobody can upload tags or petitions.
- **Privacy changes.** A self-hosted Hyd keeps hash queries on your machine.
  A public Hyd sees every hash and tag a client asks for. The PTR
  download-everything model exists to avoid exactly that central query log.
  State your logging/retention policy or disable request logging.
- **Bandwidth and storage.** The relay needs `keep_update_cache = true` and a
  complete cache (roughly another 21 GiB), and each mirror client may download
  several GiB. Keep `max_connections`, the rate limits, and a bandwidth budget
  in mind.
- **Auth.** There are shared access keys, not per-user accounts. A key can be
  revoked by removing it and restarting. Loopback trust is dangerous behind a
  reverse proxy: a public proxy connecting from localhost looks trusted.
- **TLS.** Hyd speaks plain HTTP. Terminate TLS in a reverse proxy and keep the
  Hyd listener bound to loopback or a private interface.
- **No media and no writes.** Hyd stores no files, so media hosting and
  copyright questions are out of scope, but it also cannot replace the social
  and moderation side of the PTR.

## Scaling with a mirror fleet

A useful topology is: the PTR stays authoritative, several Hyd instances serve
the update stream, and clients download from the nearest mirror while writes
still go to the PTR.

```
                 writes (tags, petitions)
client  -------------------------------->  PTR (authoritative)
   \                                          ^
    \  GET /metadata, GET /update            | mirrors sync once
     v                                        |
  hyd mirror A   hyd mirror B   hyd mirror C -+
```

Why it fits:

- Update files are immutable and hash-addressed, so mirrors are natural caches
  and can be seeded from the PTR or from each other.
- Clients still download everything and keep their own database, so there is no
  central query oracle and the Hydrus privacy model is unchanged.
- The large transfer is the initial update stream, not hash lookups. Offloading
  it removes most PTR egress. Offloading the `/hyd/*` query endpoints is a
  separate, smaller win for third-party tools.
- Writes stay centralized, moderated, and rate-limited on the PTR.

What is still missing for this to be smooth:

- **Two URLs per service.** A Hydrus repository service stores one URL, one
  access key, and one service key. "Download from a mirror, write to the PTR"
  needs either a client-side `download_url`/`mirror_of` field with the PTR as
  the write target, or a way to attach a mirror to an existing service. Adding
  a second service does not work well because the service key identifies the
  repository and clients key services by that key. hydd itself already
  implements the split for its own sync: `repository_url` is the bulk download
  source and `trust_root` (default: the PTR) supplies the authoritative update
  hashes. A lagging mirror only causes per-file fallback to the trust root; a
  mirror that serves bytes not matching the trust root aborts the sync loudly
  so the operator can switch instances, and a trust root that rewrites an
  already-published index aborts the same way.
- **Freshness and trust.** A mirror can lag. The strongest design fetches the
  small `/metadata` response from the authoritative PTR and the large
  `/update` bodies from a mirror, verifying each file against the hash in that
  metadata. A client that trusts the mirror for both gets the mirror's
  freshness and trust level. Repository metadata is not signed, so a mirror is
  a trust choice, not a cryptographic guarantee.
- **Discovery and failover.** Clients need a mirror list or a DNS/round-robin
  entry point, plus a fallback to the PTR and a way to avoid a stale mirror.
- **Operations.** Each relay mirror needs a complete cache (`keep_update_cache`,
  roughly 21 GiB), TLS, rate limits, lag monitoring, and enough egress. A
  store is optional for relay-only mirrors; query endpoints need the store.

This is the part worth taking to the Hydrus developer alongside the read-only
profile: a read-only service that advertises its upstream identity, supports a
separate download source, and never accepts writes.

## Bootstrap from a mirror, then switch to the PTR

Until a client supports separate download and write URLs, a simpler workflow is
enough: add a complete Hyd mirror as the repository, let it download the bulk of
the stream, then edit the repository URL and access key to point at the real
PTR. The client keeps its local database and update index, so switching hosts
only fetches whatever the PTR has that the mirror did not, and writes continue
against the PTR as normal.

Requirements:

- The mirror must present the **same service key, type, and name** as the PTR,
  and serve the exact same update stream. Otherwise the client sees a different
  repository and the tags do not line up.
- The mirror's cache must be complete (`keep_update_cache = true`), or
  `/metadata` returns 503 and the initial sync stalls.
- The mirror must implement the same read-only repository envelope as above
  (`/service_info`, session cookie, `/metadata`, `GET /update`, account/options).
  Without it the client cannot add even the mirror.
- The user changes both the URL and the access key to the PTR's public values,
  and trusts the mirror for the data it seeded. A mirror that served a divergent
  stream would leave the client in a state the PTR cannot reconcile.

This removes the heavy initial transfer from the PTR without needing any new
client feature, which makes it the practical first step. If the mirror's
`next_update_due` is still in the future after the switch, trigger the client's
check-for-updates action so it does not wait for the old schedule. The cost is
that every user still switches manually, and the mirror must be trustworthy and
current enough to be a useful seed. A future `download_url`/`mirror_of` field
would make the same topology automatic.

## If your client supports both Hydrus and Hyd

Model them as two backends, not one:

- **Local Hydrus client API**: usually `http://127.0.0.1:45869`, full local
  database, files, thumbnails, search, imports, tags. Works only where that
  client runs.
- **Hyd remote API**: `http(s)://host:port`, read-only hash/tag lookups and paged
  reverse search. No files, no thumbnails, no search-by-metadata, no imports.

Both can return the same tag shapes for a known hash, but only the local client
can answer "show me the file", "search my library", or "add this tag". A booru
client that only needs hash-to-tag lookups can use either; one that needs media
needs a Hydrus client or another media source.

## See also

- [api.md](api.md) - Hyd's endpoint reference
- [examples.md](examples.md) - runnable queries
- `man hydd` - daemon options
- Hydrus developer docs: <https://hydrusnetwork.github.io/hydrus/developer_api.html>
