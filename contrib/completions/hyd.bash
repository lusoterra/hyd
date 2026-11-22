# bash completion for hyd
_hyd()
{
    local cur
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"

    local commands="sha256 sha1 md5 hash tag query lookup stats scan verify version help"
    local flags="--sha256 --sha1 --md5 --tag --lookup --stats --scan --verify --version --help"

    if [ "${COMP_CWORD}" -eq 1 ]; then
        COMPREPLY=( $(compgen -W "${commands} ${flags}" -- "${cur}") )
        return 0
    fi

    case "${COMP_WORDS[1]}" in
        sha256|sha1|md5|hash|tag|query|--sha256|--sha1|--md5|--tag)
            COMPREPLY=( $(compgen -f -- "${cur}") )
            ;;
        *)
            ;;
    esac
    return 0
}
complete -F _hyd hyd
