set -l hyd_commands stats scan verify version help

complete -c hyd -f

complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a stats -d 'Show repository statistics'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a scan -d 'Scan cached update files'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a repair -d 'Re-download missing or corrupt cached update files'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a verify -d 'Open and validate the store'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a version -d 'Print the hyd version'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a help -d 'Show usage'

complete -c hyd -l sha256 -d 'Hash files with SHA-256 (default)'
complete -c hyd -l sha1 -d 'Hash files with SHA-1'
complete -c hyd -l md5 -d 'Hash files with MD5'
complete -c hyd -l help -d 'Show usage'
complete -c hyd -l version -d 'Print the hyd version'

complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -F
