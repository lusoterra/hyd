complete -c hydd -s c -l config -d 'Configuration file' -r
complete -c hydd -s d -l data-dir -d 'Override data directory' -r -a '(__fish_complete_directories)'
complete -c hydd -s l -l listen -d 'Override API listen address' -r
complete -c hydd -s p -l port -d 'Override API listen port' -r
complete -c hydd -l no-sync -d 'Serve the existing store without syncing'
complete -c hydd -l sync-once -d 'Run one sync pass and exit'
complete -c hydd -l query -d 'Print tags for a SHA-256 hash and exit' -r
complete -c hydd -l compact -d 'Merge pending deltas into the base store and exit'
complete -c hydd -l verify-store -d 'Open and validate the store and exit'
complete -c hydd -l print-access-key -d 'Print the API access key and exit'
complete -c hydd -l log-level -d 'Log verbosity' -r -a 'trace debug info warn error off'
complete -c hydd -s h -l help -d 'Show help'
complete -c hydd -s V -l version -d 'Show version'
