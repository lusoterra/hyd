#compdef hydd

_arguments -s -S \
    '(-c --config)'{-c,--config}'[configuration file]:file:_files' \
    '(-d --data-dir)'{-d,--data-dir}'[override data directory]:directory:_directories' \
    '(-l --listen)'{-l,--listen}'[override API listen address]:address:' \
    '(-p --port)'{-p,--port}'[override API listen port]:port:' \
    '--no-sync[serve the existing store without syncing]' \
    '--sync-once[run one sync pass and exit]' \
    '--query[print tags from the local store for a file hash and exit]:hash:' \
    '--api-query[query the running API for a file hash and exit]:hash:' \
    '--compact[merge pending deltas into the base store and exit]' \
    '--verify-store[open and validate the store and exit]' \
    '--scan-updates[parse every cached update file and report format errors]' \
    '--stats[query the running local API and print repository statistics]' \
    '--print-access-key[print the API access key and exit]' \
    '--log-level[log verbosity]:level:(trace debug info warn error off)' \
    '(-h --help)'{-h,--help}'[show help]' \
    '(-V --version)'{-V,--version}'[show version]'
