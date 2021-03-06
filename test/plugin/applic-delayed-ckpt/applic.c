/* NOTE: This file must be compiled with -fPIC in order to work properly.
 *
 *       The code in this file will work both with and without DMTCP.
 *       Of course, the dmtcp.h file is needed in both cases.
 *
 * These functions are in <DMTCP_ROOT>/lib/dmtcp/libdmtcp.so and dmtcp.h
 *   int dmtcpIsEnabled() - returns 1 when running with DMTCP; 0 otherwise.
 *   int dmtcpCheckpoint() - returns DMTCP_AFTER_CHECKPOINT,
 *                                   DMTCP_AFTER_RESTART, or DMTCP_NOT_PRESENT.
 * These return 0 on success and DMTCP_NOT_PRESENT if DMTCP is not present.
 *   int dmtcpDelayCheckpointsLock() - DMTCP will block any checkpoint requests.
 *   int dmtcpDelayCheckpointsUnlock() - DMTCP will execute any blocked
 *               checkpoint requests, and will permit new checkpoint requests.
 *
 * FOR ADVANCED USERS, ONLY:
 *   dmtcpGetLocalStatus
 *   dmtcpGetCoordinatorStatus
 *   dmtcpInstallHooks
 *   dmtcpRunCommand
 */

#include <stdio.h>
#include <stdlib.h>

#include "dmtcp.h"
;
int main() {
    if ( ! dmtcp_is_enabled() ) {
      printf("\n *** dmtcpIsEnabled: executable seems to not be running"
             " under dmtcp_launch.\n");
    }

    int retval = dmtcp_disable_ckpt();
    if (retval == DMTCP_NOT_PRESENT) {
      printf("\n *** dmtcpDelayCheckpointsLock: DMTCP_NOT_PRESENT."
             "  Will exit.\n");
      exit(1);
    }
    printf("*** dmtcpDelayCheckpointsLock: Checkpoints are blocked.\n");
    printf("      But a checkpoint was requested asynchronously\n"
           "      using 'dmtcp_command -i 2' from the command line.\n");
    printf("*** sleep: sleeping 3 seconds.\n\n");
    sleep(3);
    printf("*** dmtcpDelayCheckpointsUnlock: Will now unblock checkpointing\n"
           "      and write ./dmtcp_restart_script.sh.\n");
    printf("*** Execute ./dmtcp_restart_script.sh to restart from here.\n");
    dmtcp_enable_ckpt();
    sleep(2); // Wait long enough for checkpoint to be written.

    printf("\n*** Process done executing.  Successfully exiting.\n");
    return 0;
}
