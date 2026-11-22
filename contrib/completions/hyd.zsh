#compdef hyd

_hyd()
{
    local -a commands
    commands=(
        'stats:show daemon, repository and build statistics'
        'scan:parse every cached update file and report problems'
        'verify:open and validate the local store'
        'version:print the hyd version'
        'help:show usage'
    )

    _arguments -C \
        '--sha256[hash files with SHA-256 (default)]' \
        '--sha1[hash files with SHA-1]' \
        '--md5[hash files with MD5]' \
        '(-h --help)'{-h,--help}'[show usage]' \
        '(-V --version)'{-V,--version}'[show version]' \
        '1:file or hash:->target' \
        '*::argument:->argument'

    case $state in
        target)
            _alternative \
                'commands:command:compadd -a commands' \
                'files:file:_files'
            ;;
        argument)
            _files
            ;;
    esac
}

_hyd "$@"
