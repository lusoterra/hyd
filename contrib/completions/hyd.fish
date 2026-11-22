set -l hyd_commands sha256 sha1 md5 hash tag query lookup stats scan verify version help

complete -c hyd -f

complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a sha256 -d 'Print the SHA-256 hash of each file'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a sha1 -d 'Print the SHA-1 hash of each file'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a md5 -d 'Print the MD5 hash of each file'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a hash -d 'Alias for sha256'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a tag -d 'Print the PTR tags for each file'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a query -d 'Alias for tag'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a lookup -d 'Print the tags for one or more hashes'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a stats -d 'Show repository statistics'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a scan -d 'Scan cached update files'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a verify -d 'Open and validate the store'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a version -d 'Print the hydd version'
complete -c hyd -n "not __fish_seen_subcommand_from $hyd_commands" -a help -d 'Show usage'

complete -c hyd -l sha256 -d 'Alias for sha256'
complete -c hyd -l sha1 -d 'Alias for sha1'
complete -c hyd -l md5 -d 'Alias for md5'
complete -c hyd -l tag -d 'Alias for tag'
complete -c hyd -l lookup -d 'Alias for lookup'
complete -c hyd -l stats -d 'Alias for stats'
complete -c hyd -l scan -d 'Alias for scan'
complete -c hyd -l verify -d 'Alias for verify'
complete -c hyd -l version -d 'Alias for version'
complete -c hyd -l help -d 'Show usage'

complete -c hyd -n "__fish_seen_subcommand_from sha256 sha1 md5 hash tag query" -F
complete -c hyd -n "__fish_seen_subcommand_from --sha256 --sha1 --md5 --tag" -F
