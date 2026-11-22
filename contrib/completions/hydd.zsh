#compdef hydd

_arguments -s -S \
    '(-c --config)'{-c,--config}'[configuration file]:file:_files' \
    '(-d --data-dir)'{-d,--data-dir}'[override data directory]:directory:_directories' \
    '(-l --listen)'{-l,--listen}'[override API listen address]:address:' \
    '(-p --port)'{-p,--port}'[override API listen port]:port:' \
    '--no-sync[serve the existing store without syncing]' \
    '--sync-once[run one sync pass and exit]' \
    '--query[print tags for a file hash (SHA-256, SHA-1, or MD5) and exit]:hash:' \
    '--compact[merge pending deltas into the base store and exit]' \
    '--verify-store[open and validate the store and exit]' \
    '--print-access-key[print the API access key and exit]' \
    '--log-level[log verbosity]:level:(trace debug info warn error off)' \
    '(-h --help)'{-h,--help}'[show help]' \
    '(-V --version)'{-V,--version}'[show version]'
