/*********************************************************
 * Copyright (C) 2012 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*********************************************************
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of VMware Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission of VMware Inc.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *********************************************************/

/*********************************************************
 * The contents of this file are subject to the terms of the Common
 * Development and Distribution License (the "License") version 1.0
 * and no later version.  You may not use this file except in
 * compliance with the License.
 *
 * You can obtain a copy of the License at
 *         http://www.opensource.org/licenses/cddl1.php
 *
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 *********************************************************/
#include "backdoor_balloon.h"
#include "backdoor.h"
#include "balloon_def.h"
#include "os.h"

/*
 *----------------------------------------------------------------------
 *
 * BackdoorBalloon --
 *
 *      Do the balloon hypercall to the vmkernel.
 *
 * Results:
 *      vmkernel response returned in myBp.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static
void BackdoorBalloon(Backdoor_proto *myBp) // IN/OUT
{
   myBp->in.ax.word = BALLOON_BDOOR_MAGIC;
   myBp->in.dx.halfs.low = BALLOON_BDOOR_PORT;
   Backdoor_InOut(myBp);
}

/*
 *----------------------------------------------------------------------
 *
 * Backdoor_MonitorGetProto --
 *
 *      Get the best protocol to communicate with the host.
 *
 * Results:
 *      Returns BALLOON_SUCCESS if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Backdoor_MonitorGetProto(Balloon *b) // IN
{
   uint32 status;
   Backdoor_proto bp;

   bp.in.cx.halfs.low = BALLOON_BDOOR_CMD_GET_PROTO_V3;

   BackdoorBalloon(&bp);

   status = bp.out.ax.word;
   if (status == BALLOON_SUCCESS) {
      b->hypervisorProtocolVersion = bp.out.cx.word;
   } else if (status == BALLOON_ERROR_CMD_INVALID) {
      /*
       * Let's assume that if the GET_PROTO command doesn't exist, then
       * the hypervisor uses the v2 protocol.
       */
      b->hypervisorProtocolVersion = BALLOON_PROTOCOL_VERSION_2;
      status = BALLOON_SUCCESS;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Backdoor_MonitorStart --
 *
 *      Attempts to contact monitor via backdoor to begin operation.
 *
 * Results:
 *      Returns BALLOON_SUCCESS if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Backdoor_MonitorStart(Balloon *b) // IN
{
   uint32 status;
   Backdoor_proto bp;

   /* prepare backdoor args */
   bp.in.cx.halfs.low = BALLOON_BDOOR_CMD_START;
   bp.in.size = BALLOON_PROTOCOL_VERSION;

   /* invoke backdoor */
   BackdoorBalloon(&bp);

   /* parse return values */
   status = bp.out.ax.word;

   /*
    * If return code is BALLOON_SUCCESS_V3, then ESX is informing us
    * that CMD_GET_PROTO is available, which we can use to gather the
    * best protocol to use.
    */
   if (status == BALLOON_SUCCESS_V3) {
      status = Backdoor_MonitorGetProto(b);
   } else if (status == BALLOON_SUCCESS) {
      b->hypervisorProtocolVersion = BALLOON_PROTOCOL_VERSION_2;
   }

   /* update stats */
   STATS_INC(b->stats.start);
   if (status != BALLOON_SUCCESS) {
      STATS_INC(b->stats.startFail);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Backdoor_MonitorGuestType --
 *
 *      Attempts to contact monitor and report guest OS identity.
 *
 * Results:
 *      Returns BALLOON_SUCCESS if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Backdoor_MonitorGuestType(Balloon *b) // IN
{
   uint32 status, target;
   Backdoor_proto bp;

   /* prepare backdoor args */
   bp.in.cx.halfs.low = BALLOON_BDOOR_CMD_GUEST_ID;
   bp.in.size = b->guestType;

   /* invoke backdoor */
   BackdoorBalloon(&bp);

   /* parse return values */
   status = bp.out.ax.word;
   target = bp.out.bx.word;

   /* set flag if reset requested */
   if (status == BALLOON_ERROR_RESET) {
      b->resetFlag = 1;
   }

   /* update stats */
   STATS_INC(b->stats.guestType);
   if (status != BALLOON_SUCCESS) {
      STATS_INC(b->stats.guestTypeFail);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Backdoor_MonitorGetTarget --
 *
 *      Attempts to contact monitor via backdoor to obtain desired
 *      balloon size.
 *
 *      Predicts the maximum achievable balloon size and sends it
 *      to vmm => vmkernel via vEbx register.
 *
 *      OS_ReservedPageGetLimit() returns either predicted max balloon
 *      pages or BALLOON_MAX_SIZE_USE_CONFIG. In the later scenario,
 *      vmkernel uses global config options for determining a guest's max
 *      balloon size. Note that older vmballoon drivers set vEbx to
 *      BALLOON_MAX_SIZE_USE_CONFIG, i.e., value 0 (zero). So vmkernel
 *      will fallback to config-based max balloon size estimation.
 *
 * Results:
 *      If successful, sets "target" to value obtained from monitor,
 *      and returns BALLOON_SUCCESS. Otherwise returns error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Backdoor_MonitorGetTarget(Balloon *b,     // IN
                          uint32 *target) // OUT
{
   Backdoor_proto bp;
   unsigned long limit;
   uint32 limit32;
   uint32 status;

   limit = OS_ReservedPageGetLimit();

   /* Ensure limit fits in 32-bits */
   limit32 = (uint32)limit;
   if (limit32 != limit) {
      return BALLOON_FAILURE;
   }

   /* prepare backdoor args */
   bp.in.cx.halfs.low = BALLOON_BDOOR_CMD_TARGET;
   bp.in.size = limit;

   /* invoke backdoor */
   BackdoorBalloon(&bp);

   /* parse return values */
   status  = bp.out.ax.word;
   *target = bp.out.bx.word;

   /* set flag if reset requested */
   if (status == BALLOON_ERROR_RESET) {
      b->resetFlag = 1;
   }

   /* update stats */
   STATS_INC(b->stats.target);
   if (status != BALLOON_SUCCESS) {
      STATS_INC(b->stats.targetFail);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Backdoor_MonitorLockPage --
 *
 *      Attempts to contact monitor and add PPN corresponding to
 *      the page handle to set of "balloon locked" pages.
 *
 * Results:
 *      Returns BALLOON_SUCCESS if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Backdoor_MonitorLockPage(Balloon *b,    // IN
                         PPN ppn)       // IN
{
   uint32 ppn32;
   uint32 status, target;
   Backdoor_proto bp;

   /* Ensure PPN fits in 32-bits, i.e. guest memory is limited to 16TB. */
   ppn32 = (uint32)ppn;
   if (ppn32 != ppn) {
      return BALLOON_ERROR_PPN_INVALID;
   }

   /* prepare backdoor args */
   bp.in.cx.halfs.low = BALLOON_BDOOR_CMD_LOCK;
   bp.in.size = ppn32;

   /* invoke backdoor */
   BackdoorBalloon(&bp);

   /* parse return values */
   status = bp.out.ax.word;
   target = bp.out.bx.word;

   /* set flag if reset requested */
   if (status == BALLOON_ERROR_RESET) {
      b->resetFlag = 1;
   }

   /* update stats */
   STATS_INC(b->stats.lock);
   if (status != BALLOON_SUCCESS) {
      STATS_INC(b->stats.lockFail);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Backdoor_MonitorUnlockPage --
 *
 *      Attempts to contact monitor and remove PPN corresponding to
 *      the page handle from set of "balloon locked" pages.
 *
 * Results:
 *      Returns BALLOON_SUCCESS if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Backdoor_MonitorUnlockPage(Balloon *b,  // IN
                           PPN ppn)     // IN
{
   uint32 ppn32;
   uint32 status, target;
   Backdoor_proto bp;

   /* Ensure PPN fits in 32-bits, i.e. guest memory is limited to 16TB. */
   ppn32 = (uint32)ppn;
   if (ppn32 != ppn) {
      return BALLOON_ERROR_PPN_INVALID;
   }

   /* prepare backdoor args */
   bp.in.cx.halfs.low = BALLOON_BDOOR_CMD_UNLOCK;
   bp.in.size = ppn32;

   /* invoke backdoor */
   BackdoorBalloon(&bp);

   /* parse return values */
   status = bp.out.ax.word;
   target = bp.out.bx.word;

   /* set flag if reset requested */
   if (status == BALLOON_ERROR_RESET) {
      b->resetFlag = 1;
   }

   /* update stats */
   STATS_INC(b->stats.unlock);
   if (status != BALLOON_SUCCESS) {
      STATS_INC(b->stats.unlockFail);
   }

   return status;
}