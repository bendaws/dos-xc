# Run build steps in the background, at most JOBS at a time.  Sourced by the
# build scripts.  Set JOBS to override the processor count.
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
_spawn_pids=
_spawn_count=0

# Wait for every started command.  Exits if any of them failed.
finish_jobs() {
    _spawn_failed=0
    for _spawn_pid in $_spawn_pids; do
        wait "$_spawn_pid" || _spawn_failed=1
    done
    _spawn_pids=
    _spawn_count=0
    if [ "$_spawn_failed" != 0 ]; then
        echo "error: a parallel build step failed" >&2
        exit 1
    fi
}

# Start a command.  Once JOBS are running, wait for that batch to finish.
spawn() {
    "$@" &
    _spawn_pids="$_spawn_pids $!"
    _spawn_count=$((_spawn_count + 1))
    [ "$_spawn_count" -lt "$JOBS" ] || finish_jobs
}
