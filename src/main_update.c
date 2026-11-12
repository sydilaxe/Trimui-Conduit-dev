/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host updater: does what the app does when you press A on the update prompt,
 * from the command line, for tests (tools/fake_github.py) and for trying a
 * release before it reaches the TrimUI.
 *
 * usage: conduit_update APP_DIR [--force]
 *   checks GitHub (or CONDUIT_UPDATE_API) for a release newer than CONDUIT_VERSION and
 *   installs it into APP_DIR, which needs cacert.pem for HTTPS */
#include <stdio.h>
#include <string.h>

#include "update.h"
#include "net.h"
#include "update.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: conduit_update APP_DIR [--force]\n");
        return 2;
    }
    const char *dir = argv[1];
    int force = argc > 2 && !strcmp(argv[2], "--force");
    char ca[1100];
    snprintf(ca, sizeof ca, "%s/cacert.pem", dir);
    net_init(ca);
    const UpdateStatus *u = update_status();
    if (update_check()) {
        fprintf(stderr, "%s\n", u->err);
        return 1;
    }
    printf("this is v" CONDUIT_VERSION ", the latest release is v%s\n", u->latest);
    if (u->state != UP_AVAILABLE && !force)
        return 0;
    if (update_install(dir)) {
        fprintf(stderr, "update failed: %s\n", u->err);
        return 1;
    }
    printf("installed v%s into %s\n", u->latest, dir);
    return 0;
}
