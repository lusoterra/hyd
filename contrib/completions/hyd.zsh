#compdef hyd

_hyd()
{
    local -a commands
    commands=(
        'stats:show daemon, repository and build statistics'
        'counts:mapping count for one or more tags'
        'hashes:page the reverse tag lookup (tags are ANDed; -tag excludes)'
        'hashes-after:continue a reverse lookup after a next_after id'
        'rebuild-counts:rebuild the derived tag counts and name index'
        'check:verify the store and the derived counts'
        'scan:parse every cached update file and report problems'
        'repair:re-download missing or corrupt cached update files'
        'verify:open and validate the local store'
        'version:print the hyd version'
        'help:show usage'
    )

    if (( CURRENT == 2 )); then
        _alternative \
            'commands:command:compadd -a commands' \
            'files:file:_files'
        return
    fi

    case "${words[2]}" in
        hashes)
            _arguments \
                '--offset[page offset]:offset:' \
                '--limit[page size 1-1000]:limit:' \
                '--after[continue after a next_after id]:id:' \
                '*:tag:'
            ;;
        counts|hashes-after)
            _arguments '*:tag:'
            ;;
        *)
            _files
            ;;
    esac
}

_hyd "$@"
