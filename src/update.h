/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-update, as in Columbia Street, RadioScan and Jaffedrone: ask GitHub for the latest release,
 * and if it is newer than CONDUIT_VERSION, download its package and swap the new files into the app
 * folder. Your settings (conduit.cfg), recordings and logs stay as they are. main.c reads the status
 * and drives it.
 *
 * The package is Conduit-mustardos.muxapp, the MustardOS application archive: one build for the
 * Brick, Brick Hammer and Brick Pro. */
#pragma once
#include <stddef.h>

#ifndef CONDUIT_VERSION
#define CONDUIT_VERSION "0.1.0"
#endif
#define UPD_REPO "sydilaxe/Trimui-Conduit-dev"
/* the release file, and the folder its zip holds the app in */
#define UPD_ASSET "Conduit-mustardos.muxapp"
#define UPD_ZIP_DIR "Conduit/" /* MustardOS unpacks a .muxapp into its application folder */

/* exit status that tells launch.sh to start the app again (the new version) */
#define RELAUNCH_STATUS 75

enum {
    UP_IDLE,         /* not checked yet */
    UP_CHECKING,
    UP_CURRENT,      /* this is the latest release */
    UP_AVAILABLE,    /* a newer release is out: latest, url */
    UP_CHECK_FAILED,
    UP_INSTALLING,   /* progress 0..100 */
    UP_INSTALLED,    /* the new files are in place: relaunch */
    UP_FAILED        /* the install failed: err (the old version is untouched) */
};

typedef struct UpdateStatus {
    volatile int state;
    volatile int progress;
    char latest[32]; /* "0.3.1" */
    char url[1024];  /* the release's update_asset() */
    char err[160];
} UpdateStatus;

UpdateStatus *update_status(void);
const char *update_asset(void); /* the release file this copy updates from */
void update_check_start(void);                  /* in the background: CHECKING -> CURRENT / AVAILABLE / CHECK_FAILED */
void update_install_start(const char *app_dir); /* from AVAILABLE or FAILED: INSTALLING -> INSTALLED / FAILED */

/* blocking versions, for tools/ and tests; 0 on success */
int update_check(void);
int update_install(const char *app_dir);

/* true if the dotted version a ("v0.3.1" or "0.3.1") is newer than b */
int version_newer(const char *a, const char *b);
/* unpack the app folder of a zip (Conduit/) into dir, keeping the user's files; 0 on success */
int update_unzip(const unsigned char *zip, long len, const char *dir, char *err, size_t errlen);
