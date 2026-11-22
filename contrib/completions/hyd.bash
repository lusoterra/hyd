# bash completion for hyd
_hyd()
{
    local cur
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"

    local commands="stats scan repair verify version help"
    local flags="--sha256 --sha1 --md5 --help --version"

    if [ "${COMP_CWORD}" -eq 1 ]; then
        COMPREPLY=( $(compgen -W "${commands} ${flags}" -- "${cur}") $(compgen -f -- "${cur}") )
        return 0
    fi

    case "${COMP_WORDS[1]}" in
        stats|scan|repair|verify|version|help)
            ;;
        *)
            COMPREPLY=( $(compgen -f -- "${cur}") )
            ;;
    esac
    return 0
}
complete -F _hyd hyd
