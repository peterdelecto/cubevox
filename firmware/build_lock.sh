# Sourced by the build scripts. build_lock_acquire LOCKDIR LABEL waits for the build that holds
# LOCKDIR, then takes it; build_lock_release gives it up. The caller calls build_lock_release from
# its EXIT trap, so the lock is freed on every exit path.
#
# The lock dir holds the owner's PID. A lock whose owner is dead is reaped under a second mkdir
# mutex, so two waiters cannot both remove it and the second then remove a lock the first
# re-created.

BUILD_LOCK_WAIT_S=600
build_lock_dir=""
build_lock_held=0
build_lock_reaping=0
build_lock_reaped=0

build_lock_release() {
  if [ "$build_lock_reaping" -eq 1 ]; then rmdir "$build_lock_dir.reap"; build_lock_reaping=0; fi
  if [ "$build_lock_held" -eq 1 ]; then
    rm -f "$build_lock_dir/pid"
    rmdir "$build_lock_dir"
    build_lock_held=0
  fi
}

# Sets build_lock_reaped=1 when it removed a stale lock. Called as a plain command so set -e
# applies inside.
build_lock_reap_stale() {
  build_lock_reaped=0
  mkdir "$build_lock_dir.reap" 2>/dev/null || return 0  # another waiter is reaping
  build_lock_reaping=1
  local owner
  owner="$(cat "$build_lock_dir/pid" 2>/dev/null || true)"  # absent while the owner is still writing it
  if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
    echo "[build] removing stale lock $build_lock_dir (pid $owner is gone)"
    rm -f "$build_lock_dir/pid"
    rmdir "$build_lock_dir"
    build_lock_reaped=1
  fi
  rmdir "$build_lock_dir.reap"
  build_lock_reaping=0
}

build_lock_acquire() {
  build_lock_dir="$1"
  local label="$2" waited=0 mkerr owner
  while true; do
    if mkerr="$(mkdir "$build_lock_dir" 2>&1)"; then break; fi
    if [ ! -d "$build_lock_dir" ]; then
      echo "[ERROR] cannot create lock $build_lock_dir: $mkerr"
      exit 1
    fi
    build_lock_reap_stale
    if [ "$build_lock_reaped" -eq 1 ]; then continue; fi
    owner="$(cat "$build_lock_dir/pid" 2>/dev/null || true)"
    if [ "$waited" -ge "$BUILD_LOCK_WAIT_S" ]; then
      echo "[ERROR] a $label build (pid ${owner:-unknown}) held $build_lock_dir for $BUILD_LOCK_WAIT_S s"
      exit 1
    fi
    if [ "$waited" -eq 0 ]; then
      echo "[build] waiting for the running $label build (pid ${owner:-unknown}, lock $build_lock_dir)"
    fi
    sleep 2
    waited=$((waited + 2))
  done
  build_lock_held=1
  echo $$ > "$build_lock_dir/pid"
}
