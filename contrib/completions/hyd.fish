set -l hyd_commands stats counts hashes hashes-after rebuild-counts check scan verify repair version help

complete -c hyd -f

complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a stats -d 'Show repository statistics'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a counts -d 'Mapping count for one or more tags'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a hashes -d 'Page the reverse tag lookup'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a hashes-after -d 'Continue a reverse lookup after a next_after id'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a rebuild-counts -d 'Rebuild the derived tag counts and name index'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a check -d 'Verify the store and the derived counts'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a scan -d 'Scan cached update files'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a repair -d 'Re-download missing or corrupt cached update files'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a verify -d 'Open and validate the store'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a version -d 'Print the hyd version'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a help -d 'Show usage'

complete -c hyd -n "__fish_seen_subcommand_from hashes" -l offset -d 'Page offset' -r
complete -c hyd -n "__fish_seen_subcommand_from hashes" -l limit -d 'Page size 1-1000' -r
complete -c hyd -n "__fish_seen_subcommand_from hashes" -l after -d 'Continue after a next_after id' -r

complete -c hyd -l sha256 -d 'Hash files with SHA-256 (default)'
complete -c hyd -l sha1 -d 'Hash files with SHA-1'
complete -c hyd -l md5 -d 'Hash files with MD5'
complete -c hyd -l help -d 'Show usage'
complete -c hyd -l version -d 'Print the hyd version'

complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -F
