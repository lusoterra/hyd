complete -c hydd -s c -l config -d 'Configuration file' -r
complete -c hydd -s d -l data-dir -d 'Override data directory' -r -a '(__fish_complete_directories)'
complete -c hydd -s l -l listen -d 'Override API listen address' -r
complete -c hydd -s p -l port -d 'Override API listen port' -r
complete -c hydd -l no-sync -d 'Serve the existing store without syncing'
complete -c hydd -l sync-once -d 'Run one sync pass and exit'
complete -c hydd -l query -d 'Print tags for a file hash (SHA-256, SHA-1, or MD5) and exit' -r
complete -c hydd -l api-query -d 'Query the running API for a file hash and exit' -r
complete -c hydd -l compact -d 'Merge pending deltas into the base store and exit'
complete -c hydd -l verify-store -d 'Open and validate the store and exit'
complete -c hydd -l scan-updates -d 'Parse every cached update file and report format errors'
complete -c hydd -l repair-cache -d 'Re-download missing or corrupt cached update files and exit'
complete -c hydd -l stats -d 'Query the running local API and print repository statistics'
complete -c hydd -l rebuild-tag-counts -d 'Rebuild the derived tag counts and name index and exit'
complete -c hydd -l check-tag-counts -d 'Verify the derived tag counts and name index and exit'
complete -c hydd -l tag-counts -d 'Query the running API for mapping counts' -r
complete -c hydd -l tag-hashes -d 'Query the running API for hashes matching tags'
complete -c hydd -l hash-offset -d 'Page offset for --tag-hashes' -r
complete -c hydd -l hash-after -d 'Continue after a next_after hash id' -r
complete -c hydd -l hash-limit -d 'Page size for --tag-hashes' -r
complete -c hydd -l print-access-key -d 'Print every configured API access key and exit'
complete -c hydd -l log-level -d 'Log verbosity' -r -a 'trace debug info warn error off'
complete -c hydd -s h -l help -d 'Show help'
complete -c hydd -s V -l version -d 'Show version'
