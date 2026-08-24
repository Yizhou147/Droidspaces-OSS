/*
 * Droidspaces v6 - Media Decode Daemon and Socket Manager
 *
 * Manages the decode-daemon lifecycle on Android (spawning, logging, and
 * stopping) and host-to-container socket bridging.  The daemon exposes the
 * platform's MediaCodec hardware decoders over a small line protocol, and a
 * VA-API driver inside the container dials it, so unmodified Linux media
 * applications get hardware decode.
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "droidspace.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* daemon child */

struct decode_args {
  char sock_dir[PATH_MAX];
  char bin[PATH_MAX];
};

/* ready_fd: O_CLOEXEC write-end; EOF on execv success, byte on failure */
static void decode_child_wrapper(int ready_fd, void *user_data) {
  struct decode_args *args = (struct decode_args *)user_data;

  ds_daemon_child_preamble();

  /* Enter droidspacesd domain. The daemon talks to MediaCodec, which needs a
   * domain the media stack accepts; ksu or magisk contexts get denied. */
  ds_selinux_enter_domain();

  fprintf(stdout, "[MediaDecode] uid=%d sock_dir=%s\n", (int)getuid(),
          args->sock_dir);
  fflush(stdout);

  /* --sock takes a directory: the daemon creates decode.sock inside it, so a
   * daemon restart never invalidates the container's bind mount. */
  char sock_flag[] = "--sock";
  char *argv[] = {
      args->bin,
      sock_flag,
      args->sock_dir,
      NULL,
  };

  execv(argv[0], argv);
  perror("[MediaDecode] execv");
  if (write(ready_fd, "\x01", 1) < 0) { /* ignore */
  }
  _exit(1);
}

/* spawn */

static pid_t spawn_decode(const char *sock_dir) {
  struct decode_args args;
  safe_strncpy(args.sock_dir, sock_dir, sizeof(args.sock_dir));
  safe_strncpy(args.bin, DS_DECODE_BIN, sizeof(args.bin));
  return ds_spawn_daemon(decode_child_wrapper, &args, "mediadecode.log",
                         "MediaDecode", "MediaDecode");
}

/* Host-side socket directory, inside the workspace so it follows the Android
 * and Linux root switch instead of being hardcoded. */
static const char *decode_sock_dir(void) {
  static char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s/%s", get_workspace_dir(), DS_DECODE_SUBDIR);
  return dir;
}

static const char *decode_sock_path(void) {
  static char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", decode_sock_dir(), DS_DECODE_SOCK_NAME);
  return path;
}

/* public API */

int ds_decode_daemon_start(struct ds_config *cfg) {
  if (!cfg || !cfg->media_decode || !is_android())
    return -1;
  if (getuid() != 0) {
    ds_error("[MediaDecode] not running as root");
    return -1;
  }

  if (access(DS_DECODE_BIN, X_OK) != 0) {
    ds_warn("MediaDecode: daemon binary not found at %s - skipping start",
            DS_DECODE_BIN);
    return -1;
  }

  /* Reuse existing global daemon if still alive */
  pid_t existing = ds_daemon_read_pid("mediadecode.dpid");
  if (existing > 0) {
    ds_log("MediaDecode: daemon already running (PID %d)", (int)existing);
    cfg->decode_pid = existing;
    return 1;
  }

  const char *dir = decode_sock_dir();
  if (mkdir_p(dir, 0755) < 0) {
    ds_warn("MediaDecode: cannot create %s: %s", dir, strerror(errno));
    return -1;
  }

  /* Clear leftovers from a crashed run. The daemon refuses to start while its
   * lock file looks held, and a stale socket inode would break the bridge. */
  unlink(decode_sock_path());
  char lock[PATH_MAX];
  snprintf(lock, sizeof(lock), "%s.lock", decode_sock_path());
  unlink(lock);

  ds_log("[MediaDecode] launching daemon (uid=%d)", (int)getuid());
  pid_t child = spawn_decode(dir);
  if (child <= 0)
    return -1;

  cfg->decode_pid = child;
  ds_daemon_write_pid("mediadecode.dpid", child);
  return 0;
}

void ds_decode_daemon_stop(struct ds_config *cfg) {
  if (!cfg)
    return;
  ds_global_daemon_stop(check_decode_needs, cfg->decode_pid, &cfg->decode_pid,
                        "mediadecode.dpid", decode_sock_path(),
                        "[MediaDecode]");
}

/* socket bridge */

int ds_setup_decode_socket(struct ds_config *cfg) {
  if (!is_android() || !cfg->media_decode)
    return 0;

  /* Post-pivot_root the host workspace is reachable under /.old_root, so build
   * the source path from there rather than from the live workspace helper. */
  char src[PATH_MAX];
  snprintf(src, sizeof(src), "%s%s/%s/%s", DS_OLDROOT_PREFIX,
           DS_WORKSPACE_ANDROID, DS_DECODE_SUBDIR, DS_DECODE_SOCK_NAME);

  struct stat st;
  if (stat(src, &st) != 0) {
    ds_warn("MediaDecode: socket not found at %s - skipping socket bridge",
            src);
    return 0;
  }

  if (ds_bind_mount_socket(src, DS_DECODE_SOCKET, st.st_uid, "MediaDecode") < 0)
    return 0;

  ds_log("MediaDecode: socket bind-mounted into container");

  /* The VA-API driver probes /run/dmd/decode.sock by default, so point it at
   * our path explicitly instead of patching the driver. */
  setenv("DMD_ENDPOINT", "unix:" DS_DECODE_SOCKET, 1);
  return 0;
}
