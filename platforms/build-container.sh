# Sourced by the Kobo and Android build scripts. The caller sets engine.
remove_build_container() {
    local result=$?
    trap - EXIT INT TERM
    if [[ -s "$container_run_dir/container.cid" ]]; then
        if ! "$engine" rm -f "$(cat "$container_run_dir/container.cid")" > /dev/null 2>&1; then
            echo "Cannot remove the build container; its ID is retained in $container_run_dir/container.cid" >&2
            exit 1
        fi
    fi
    rm -rf "$container_run_dir"
    exit "$result"
}

# Keep the container ID until removal succeeds, including on interruption.
own_build_container() {
    container_run_dir=$(mktemp -d "$1")
    trap remove_build_container EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
}
