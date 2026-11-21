# bash completion for hydd
_hydd()
{
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "${prev}" in
        -c|--config)
            COMPREPLY=( $(compgen -f -- "${cur}") )
            return 0
            ;;
        -d|--data-dir|-l|--listen)
            COMPREPLY=( $(compgen -d -- "${cur}") )
            return 0
            ;;
        -p|--port)
            return 0
            ;;
        --log-level)
            COMPREPLY=( $(compgen -W "trace debug info warn error off" -- "${cur}") )
            return 0
            ;;
        --query)
            return 0
            ;;
    esac

    opts="-c --config -d --data-dir -l --listen -p --port --no-sync --sync-once \
          --query --compact --verify-store --print-access-key --log-level -h --help -V --version"

    COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
    return 0
}
complete -F _hydd hydd
