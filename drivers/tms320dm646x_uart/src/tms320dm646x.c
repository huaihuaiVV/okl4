/*
 * Copyright (c) 2008 Open Kernel Labs, Inc. (Copyright Holder).
 * All rights reserved.
 *
 * 1. Redistribution and use of OKL4 (Software) in source and binary
 * forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 *     (a) Redistributions of source code must retain this clause 1
 *         (including paragraphs (a), (b) and (c)), clause 2 and clause 3
 *         (Licence Terms) and the above copyright notice.
 *
 *     (b) Redistributions in binary form must reproduce the above
 *         copyright notice and the Licence Terms in the documentation and/or
 *         other materials provided with the distribution.
 *
 *     (c) Redistributions in any form must be accompanied by information on
 *         how to obtain complete source code for:
 *        (i) the Software; and
 *        (ii) all accompanying software that uses (or is intended to
 *        use) the Software whether directly or indirectly.  Such source
 *        code must:
 *        (iii) either be included in the distribution or be available
 *        for no more than the cost of distribution plus a nominal fee;
 *        and
 *        (iv) be licensed by each relevant holder of copyright under
 *        either the Licence Terms (with an appropriate copyright notice)
 *        or the terms of a licence which is approved by the Open Source
 *        Initative.  For an executable file, "complete source code"
 *        means the source code for all modules it contains and includes
 *        associated build and other files reasonably required to produce
 *        the executable.
 *
 * 2. THIS SOFTWARE IS PROVIDED ``AS IS'' AND, TO THE EXTENT PERMITTED BY
 * LAW, ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED.  WHERE ANY WARRANTY IS
 * IMPLIED AND IS PREVENTED BY LAW FROM BEING DISCLAIMED THEN TO THE
 * EXTENT PERMISSIBLE BY LAW: (A) THE WARRANTY IS READ DOWN IN FAVOUR OF
 * THE COPYRIGHT HOLDER (AND, IN THE CASE OF A PARTICIPANT, THAT
 * PARTICIPANT) AND (B) ANY LIMITATIONS PERMITTED BY LAW (INCLUDING AS TO
 * THE EXTENT OF THE WARRANTY AND THE REMEDIES AVAILABLE IN THE EVENT OF
 * BREACH) ARE DEEMED PART OF THIS LICENCE IN A FORM MOST FAVOURABLE TO
 * THE COPYRIGHT HOLDER (AND, IN THE CASE OF A PARTICIPANT, THAT
 * PARTICIPANT). IN THE LICENCE TERMS, "PARTICIPANT" INCLUDES EVERY
 * PERSON WHO HAS CONTRIBUTED TO THE SOFTWARE OR WHO HAS BEEN INVOLVED IN
 * THE DISTRIBUTION OR DISSEMINATION OF THE SOFTWARE.
 *
 * 3. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR ANY OTHER PARTICIPANT BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "tms320dm646x.h"
#if !defined(NANOKERNEL)
#include <l4/kdebug.h>
#endif

#define BAUD_RATE    115200
#define UART0_REF_CLK 48000000
#define DATA_SIZE    8
#define DEFAULT_PARITY  PARITY_NONE
#define DEFAULT_STOP    1

static void do_xmit_work(struct tms320dm646x_uart *device);
static int do_rx_work(struct tms320dm646x_uart *device);

static int
device_setup_impl(struct device_interface *di, struct tms320dm646x_uart *device,
        struct resource *resources)
{
    /* FIXME: This can be autogenerated, so we should do that */
    int i, n_mem = 0;
    for (i = 0; i < 8; i++)
    {
        switch (resources->type)
        {
        case MEMORY_RESOURCE:
            if (n_mem == 0)
                device->main = *resources;
            else
                printf("tms320dm646x: got more memory than expected!\n");
            n_mem++;
            break;

        case INTERRUPT_RESOURCE:
        case BUS_RESOURCE:
        case NO_RESOURCE:
            /* do nothing */
            break;

        default:
            printf("tms320dm646x: Invalid resource type %d!\n", resources->type);
            break;
        }
        resources++;
    }

    device->tx.device = device;
    device->rx.device = device;
    device->tx.ops = stream_ops;
    device->rx.ops = stream_ops;

    /* The Baud Rate is set by the kernel.
     *  Use the same.
     */

    /* Set the character length to 8 */
    lcr_set_chlen(0x3);

    /* Set stop bits to 1 */
    lcr_set_nbstop(0x0);

    /* Set parity to NONE */
    lcr_set_paren(0x0);

    /* Disable interrupts */
    efr_set_ehen(0x1);
    lcr_set_diven(0x0);

    /* Enable the RHR interrupt */
    ier_set_rhri(0x1);

    /* Disable the THR interrupt */
    ier_set_thri(0x0);

    /* Enable the UART */
    mdro_set_modesel(0x0);
    
    return DEVICE_SUCCESS;
}

static int
device_enable_impl(struct device_interface *di, struct tms320dm646x_uart *device)
{
    device->state = STATE_ENABLED;
    return DEVICE_ENABLED;
}

static int
device_disable_impl(struct device_interface *di, struct tms320dm646x_uart *device)
{
    /* We don't actually turn off the UART here, as the kernel may still
       be using it */
    device->state = STATE_DISABLED;
    return DEVICE_DISABLED;
}

static void
do_xmit_work(struct tms320dm646x_uart *device)
{
    struct stream_interface *si = &device->tx;
    struct stream_pkt *packet = stream_get_head(si);

    if (packet == NULL) {
        return;
    }

    assert(packet->data);

    while(lsr_get_txfifoe()){
        /* Place the character into the UART FIFO */
        thr_write(packet->data[packet->xferred++]);
        assert(packet->xferred <= packet->length);
        if (packet->xferred == packet->length) {
            /* Finished this packet */
            packet = stream_switch_head(si);
            if (packet == NULL) {
                break;
            }
        }
    }
    ier_set_thri(1);
}

static int 
do_rx_work(struct tms320dm646x_uart *device)
{
    int retval = 0;
    struct stream_interface *si = &device->rx;
    struct stream_pkt *packet = stream_get_head(si);

    /* Receive as many charaters as possible */

    while (packet && !lsr_get_rxfifoe()){
        uint16_t data = rhr_get_data();
        if (data) {
            L4_KDB_Enter("data is valid");
        }

#if !defined(NANOKERNEL)
        /* Under L4, check for the magical KDB Enter keystroke. */
        if ((data & 0xff) == 0xb /* Ctrl-k */){
            L4_KDB_Enter("breakin");
            // Dont want to pass the C-k to the client
            continue;
        }
#endif

        packet->data[packet->xferred] = (data & 0xff);
        packet->xferred++;

        if (packet->xferred == packet->length) {
            packet = stream_switch_head(si);
            if (packet == NULL) {
                while(!lsr_get_rxfifoe())
                    rhr_get_data();

                break;
            }
        }
    }

    if (packet){
        ier_set_rhri(1);   /* restart the interrupt */
        if (packet->xferred){
            packet = stream_switch_head(si);
        }
    }

    return retval;
}

static int
device_interrupt_impl(struct device_interface *di, struct tms320dm646x_uart *device, int irq)
{
    int status=0;
    uint32_t r;

    r=lsr_read();

    if (r&0x1){ // RX
        ier_set_rhri(0);
        status = do_rx_work(device);
    }
    if (iir_get_it() == 0x1){ //TX
        ier_set_thri(0);   /* stop the interrupt */
        do_xmit_work(device);
    }

    return status;
}

static int
device_poll_impl(struct device_interface *di, struct tms320dm646x_uart *device)
{
    return device_interrupt_impl(di, device, -1);
}

static int
stream_sync_impl(struct stream_interface *si, struct tms320dm646x_uart *device)
{
    int retval = 0;

    if (si == &device->tx) {
        do_xmit_work(device);
    }
    if (si == &device->rx) {
        retval = do_rx_work(device);
    }
    return retval;
}

/* FIXME: This can be autogenerated! */
static int
device_num_interfaces_impl(struct device_interface *di, struct tms320dm646x_uart *device)
{
    return 2;
}

/* FIXME: This can definately be autogenerated */
static struct generic_interface *
device_get_interface_impl(struct device_interface *di, struct tms320dm646x_uart *device, int i)
{
    switch (i) {
    case 0:
        return (struct generic_interface *)(void *) &device->tx;
    case 1:
        return (struct generic_interface *)(void *) &device->rx;
    default:
        return NULL;
    }
}

