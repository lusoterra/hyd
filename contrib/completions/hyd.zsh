#compdef hyd

_hyd()
{
    local -a commands
    commands=(
        'sha256:print the SHA-256 hash of each file'
        'sha1:print the SHA-1 hash of each file'
        'md5:print the MD5 hash of each file'
        'hash:alias for sha256'
        'tag:print the PTR tags for each file'
        'query:alias for tag'
        'lookup:print the tags for one or more hashes'
        'stats:show daemon, repository and build statistics'
        'scan:parse every cached update file and report problems'
        'verify:open and validate the local store'
        'version:print the hydd version'
        'help:show usage'
        '--sha256:alias for sha256'
        '--sha1:alias for sha1'
        '--md5:alias for md5'
        '--tag:alias for tag'
        '--lookup:alias for lookup'
        '--stats:alias for stats'
        '--scan:alias for scan'
        '--verify:alias for verify'
        '--version:alias for version'
        '--help:show usage'
    )

    _arguments -C \
        '1:command:->command' \
        '*::argument:->argument'

    case $state in
        command)
            _describe 'command' commands
            ;;
        argument)
            local command
            command=${words[1]}
            if [[ $command == hyd ]]; then
                command=${words[2]}
            fi
            case $command in
                sha256|sha1|md5|hash|tag|query|--sha256|--sha1|--md5|--tag)
                    _files
                    ;;
                lookup|--lookup)
                    _message 'file hash'
                    ;;
            esac
            ;;
    esac
}

_hyd "$@"
