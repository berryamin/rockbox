/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2004 by Jens Arnold
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdbool.h>
#include "ata.h"
#include "kernel.h"
#include "thread.h"
#include "led.h"
#include "sh7034.h"
#include "system.h"
#include "debug.h"
#include "panic.h"
#include "usb.h"
#include "power.h"
#include "string.h"
#include "hwcompat.h"
#include "adc.h"

#include "bitswap.h"

/* use file for an MMC-based system, FIXME in makefile */
#ifdef HAVE_MMC

#define SECTOR_SIZE     512

/* Command definitions */
#define CMD_GO_IDLE_STATE        0x40  /* R1 */
#define CMD_SEND_OP_COND         0x41  /* R1 */
#define CMD_SEND_CSD             0x49  /* R1 */
#define CMD_SEND_CID             0x4A  /* R1 */
#define CMD_STOP_TRANSMISSION    0x4C  /* R1 */
#define CMD_SEND_STATUS          0x4D  /* R2 */
#define CMD_SET_BLOCKLEN         0x50  /* R1 */
#define CMD_READ_SINGLE_BLOCK    0x51  /* R1 */
#define CMD_READ_MULTIPLE_BLOCK  0x52  /* R1 */
#define CMD_SET_BLOCK_COUNT      0x57  /* R1 */
#define CMD_WRITE_BLOCK          0x58  /* R1b */
#define CMD_WRITE_MULTIPLE_BLOCK 0x59  /* R1b */
#define CMD_READ_OCR             0x7A  /* R3 */

/* Response formats:
   R1  = single byte, msb=0, various error flags
   R1b = R1 + busy token(s)
   R2  = 2 bytes (1st byte identical to R1), additional flags
   R3  = 5 bytes (R1 + OCR register)
*/

#define R1_PARAMETER_ERR 0x40
#define R1_ADDRESS_ERR   0x20
#define R1_ERASE_SEQ_ERR 0x10
#define R1_COM_CRC_ERR   0x08
#define R1_ILLEGAL_CMD   0x04
#define R1_ERASE_RESET   0x02
#define R1_IN_IDLE_STATE 0x01

#define R2_OUT_OF_RANGE  0x80
#define R2_ERASE_PARAM   0x40
#define R2_WP_VIOLATION  0x20
#define R2_CARD_ECC_FAIL 0x10
#define R2_CC_ERROR      0x08
#define R2_ERROR         0x04
#define R2_ERASE_SKIP    0x02
#define R2_CARD_LOCKED   0x01

// DEBUG
#include "../../apps/screens.h"

/* for compatibility */
bool old_recorder = false; /* FIXME: get rid of this cross-dependency */
int ata_spinup_time = 0;
char ata_device = 0; /* device 0 (master) or 1 (slave) */
int ata_io_address = 0; /* 0x300 or 0x200, only valid on recorder */
long last_disk_activity = -1;

/* private variables */

static struct mutex ata_mtx;

static char mmc_stack[DEFAULT_STACK_SIZE];
static const char mmc_thread_name[] = "mmc";
static struct event_queue mmc_queue;
static bool initialized = false;
static int current_card = 0;

static const unsigned char dummy[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

typedef struct
{  
    bool initialized;
    unsigned char bitrate_register;
    unsigned char rev;
    unsigned char rev_fract;
    unsigned int speed;           /* bps */
    unsigned int read_timeout;    /* n * 8 clock cycles */
    unsigned int write_timeout;   /* n * 8 clock cycles */
    unsigned int size;            /* in bytes */
    unsigned int manuf_month;
    unsigned int manuf_year;
    unsigned long serial_number;
    unsigned char name[7];
} tCardInfo;

static tCardInfo card_info[2];

/* private function declarations */

static int select_card(int card_no);
static void deselect_card(void);
static void setup_sci1(int bitrate_register);
static void write_transfer(const unsigned char *buf, int len)
            __attribute__ ((section(".icode")));
static void read_transfer(unsigned char *buf, int len)
            __attribute__ ((section(".icode")));
static unsigned char poll_byte(int timeout);
static int send_cmd(int cmd, unsigned long parameter, unsigned char *response);
static int receive_data(unsigned char *buf, int len, int timeout);
static int initialize_card(int card_no);

/* implementation */

static int select_card(int card_no)
{
    if (!card_info[card_no].initialized)
    {
        setup_sci1(7); /* Initial rate: 375 kbps (need <= 400 per mmc specs) */
        write_transfer(dummy, 10); /* allow the card to synchronize */
        while (!(SSR1 & SCI_TEND));
    }

    if (card_no == 0)
    {   /* internal */
        or_b(0x10, &PADRH);       /* set clock gate PA12  CHECKME: mask? */
        and_b(~0x04, &PADRH);     /* assert CS */
    }
    else
    {   /* external */
        and_b(~0x10, &PADRH);     /* clear clock gate PA12  CHECKME: mask?*/
        and_b(~0x02, &PADRH);     /* assert CS */
    }

    if (card_info[card_no].initialized)
    {
        setup_sci1(card_info[card_no].bitrate_register);
        return 0;
    }
    else
    {
        return initialize_card(card_no);
    }
}

static void deselect_card(void)
{
    while (!(SSR1 & SCI_TEND));   /* wait for end of transfer */
    or_b(0x06, &PADRH);           /* deassert CS (both cards) */
}

static void setup_sci1(int bitrate_register)
{
    int i;

    while (!(SSR1 & SCI_TEND));   /* wait for end of transfer */

    SCR1 = 0;                     /* disable serial port */
    SMR1 = SYNC_MODE;             /* no prescale */
    BRR1 = bitrate_register;
    SCR1 = SCI_CKE0;
    SSR1 = 0;

    for (i = 0; i <= bitrate_register; i++); /* wait at least one bit time */
    
    or_b((SCI_TE|SCI_RE), &SCR1); /* enable transmitter & receiver */
}

static void write_transfer(const unsigned char *buf, int len)
{
    const unsigned char *buf_end = buf + len;

    /* TODO: DMA */
    
    while (buf < buf_end)
    {
        while (!(SSR1 & SCI_TEND));              /* wait for end of transfer */
        TDR1 = fliptable[(signed char)(*buf++)]; /* write byte */
        SSR1 = 0;                                /* start transmitting */
    }
}

static void read_transfer(unsigned char *buf, int len)
{
    unsigned char *buf_end = buf + len;

    /* TODO: DMA */

    while (!(SSR1 & SCI_TEND));   /* wait for end of transfer */
    TDR1 = 0xFF;                  /* send do-nothing data in parallel */
    
    while (buf < buf_end)
    {
        SSR1 = 0;                                /* start receiving */
        while (!(SSR1 & SCI_RDRF));              /* wait for data */
        *buf++ = fliptable[(signed char)(RDR1)]; /* read byte */
    }
}

/* timeout is in bytes */
static unsigned char poll_byte(int timeout)
{
    int i;
    unsigned char data = 0;       /* stop the compiler complaining */

    while (!(SSR1 & SCI_TEND));   /* wait for end of transfer */
    TDR1 = 0xFF;                  /* send do-nothing data in parallel */

    i = 0;
    do {
        SSR1 = 0;                   /* start receiving */
        while (!(SSR1 & SCI_RDRF)); /* wait for data */
        data = RDR1;                /* read byte */
    } while ((data == 0xFF) && (++i < timeout));

    return fliptable[(signed char)data];
}

static unsigned char poll_busy(int timeout)
{
    int i;
    unsigned char data, dummy;
    
    while (!(SSR1 &SCI_TEND));    /* wait for end of transfer */
    TDR1 = 0xFF;                  /* send do-nothing data in parallel */

    /* get data response */
    SSR1 = 0;                     /* start receiving */
    while (!(SSR1 & SCI_RDRF));   /* wait for data */
    data = RDR1;                  /* read byte */

    /* wait until the card is ready again */
    i = 0;
    do {
        SSR1 = 0;                   /* start receiving */
        while (!(SSR1 & SCI_RDRF)); /* wait for data */
        dummy = RDR1;               /* read byte */
    } while ((dummy != 0xFF) && (++i < timeout));
    
    return fliptable[(signed char)data];
}

static int send_cmd(int cmd, unsigned long parameter, unsigned char *response)
{
    unsigned char command[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };

    command[0] = cmd;
    
    if (parameter != 0)
    {
        command[1] = (parameter >> 24) & 0xFF;
        command[2] = (parameter >> 16) & 0xFF;
        command[3] = (parameter >> 8) & 0xFF;
        command[4] = parameter & 0xFF;
    }
    
    write_transfer(command, 6);

    response[0] = poll_byte(20);
    
    if (response[0] != 0x00)
    {
        write_transfer(dummy, 1);
        return -1;
    }

    switch (cmd)
    {
        case CMD_SEND_CSD:        /* R1 response, leave open */
        case CMD_SEND_CID:
        case CMD_READ_SINGLE_BLOCK:
     // case READ_MULTIPLE_BLOCK:
            break;
            
        case CMD_SEND_STATUS:     /* R2 response, close with dummy */
            read_transfer(response + 1, 1);
            write_transfer(dummy, 1);
            break;
            
        case CMD_READ_OCR:        /* R3 response, close with dummy */
            read_transfer(response + 1, 4);
            write_transfer(dummy, 1);
            break;

        default:                  /* R1 response, close with dummy */
            write_transfer(dummy, 1);
            break;                /* also catches block writes */
    }

    return 0;
}

static int receive_data(unsigned char *buf, int len, int timeout)
{
    unsigned char crc[2];         /* unused */

    if (poll_byte(timeout) != 0xFE)
    {
        write_transfer(dummy, 1);
        return -1;                /* not start of data */
    }
        
    read_transfer(buf, len);
    read_transfer(crc, 2);        /* throw away */
    write_transfer(dummy, 1);

    return 0;
}

static int send_data(const unsigned char *buf, int len, int timeout)
{
    static const unsigned char start_data = 0xFE;
    int ret = 0;

    write_transfer(&start_data, 1);
    write_transfer(buf, len);
    write_transfer(dummy, 2);     /* crc - dontcare */

    if ((poll_busy(timeout) & 0x1F) != 0x05) /* something went wrong */
        ret = -1;

    write_transfer(dummy, 1);
    
    return ret;
}

static int initialize_card(int card_no)
{
    int i, temp;
    unsigned char response;
    unsigned char cxd[16];
    tCardInfo *card = &card_info[card_no];

    static const char mantissa[] = {  /* *10 */
        0,  10, 12, 13, 15, 20, 25, 30,
        35, 40, 45, 50, 55, 60, 70, 80
    };
    static const int speed_exponent[] = {  /* /10 */
        10000, 100000, 1000000, 10000000, 0, 0, 0, 0
    };
    
    static const int time_exponent[] = { /* reciprocal */
        1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100
    };

    /* switch to SPI mode */
    send_cmd(CMD_GO_IDLE_STATE, 0, &response);
    if (response != 0x01)
        return -1;                /* error response */

    /* initialize card */
    i = 0;
    while (send_cmd(CMD_SEND_OP_COND, 0, &response) && (++i < 200));
    if (response != 0x00)
        return -2;                /* not ready */
    
    /* get CSD register */
    if (send_cmd(CMD_SEND_CSD, 0, &response))
        return -3;
    if (receive_data(cxd, 16, 50))
        return -4;
        
    /* check block size */
    if (1 << (cxd[5] & 0x0F) != SECTOR_SIZE)
        return -5;

    /* max transmission speed the card is capable of */
    card->speed = mantissa[(cxd[3] & 0x78) >> 3]
                * speed_exponent[(cxd[3] & 0x07)];
    
    /* calculate the clock divider */
    card->bitrate_register = (FREQ/4-1) / card->speed;

    /* calculate read timeout in clock cycles from TSAC, NSAC and the actual
     * clock frequency */
    temp = (FREQ/4) / (card->bitrate_register + 1); /* actual frequency */
    card->read_timeout =
        (temp * mantissa[(cxd[1] & 0x78) >> 3] + (1000 * cxd[2]))
         / (time_exponent[cxd[1] & 0x07] * 8);
     
    /* calculate write timeout */
    temp = (cxd[12] & 0x1C) >> 2;
    if (temp > 5)
        temp = 5;
    card->write_timeout = card->read_timeout * (1 << temp);

    /* calculate size */
    card->size = ((unsigned int)(cxd[6] & 0x03) << 10)
               + ((unsigned int)cxd[7] << 2)
               + ((unsigned int)(cxd[8] & 0xC0) >> 6);
    temp = ((cxd[9] & 0x03) << 1) + ((cxd[10] & 0x80) >> 7) + 2;
    card->size *= (SECTOR_SIZE << temp);

    /* switch to full speed */
    setup_sci1(card->bitrate_register);
    
    /* get CID register */
    if (send_cmd(CMD_SEND_CID, 0, &response))
        return -6;
    if (receive_data(cxd, 16, 50))
        return -7;
    
    /* get data from CID */
    strncpy(card->name, &cxd[3], 6);
    card->name[6] = '\0';
    
    card->rev = (cxd[9] & 0xF0) >> 4;
    card->rev_fract = cxd[9] & 0x0F;

    card->manuf_month = (cxd[14] & 0xF0) >> 4;
    card->manuf_year = (cxd[14] & 0x0F) + 1997;
    
    card->serial_number = ((unsigned long)cxd[10] << 24)
                        + ((unsigned long)cxd[11] << 16)
                        + ((unsigned long)cxd[12] << 8)
                        + (unsigned long)cxd[13];

    card->initialized = true;
    return 0;
}

int ata_read_sectors(unsigned long start,
                     int incount,
                     void* inbuf)
{
    int ret = 0;
    int i;
    unsigned long addr;
    unsigned char response;
    tCardInfo *card = &card_info[current_card];
    
    addr = start * SECTOR_SIZE;
    
    mutex_lock(&ata_mtx);
    ret = select_card(current_card);

    for (i = 0; (i < incount) && (ret == 0); i++)
    {
        if ((ret = send_cmd(CMD_READ_SINGLE_BLOCK, addr, &response)))
            break;
        ret = receive_data(inbuf, SECTOR_SIZE, card->read_timeout);

        addr += SECTOR_SIZE;
        inbuf += SECTOR_SIZE;
    }
    
    deselect_card();
    mutex_unlock(&ata_mtx);

    return ret;
}


int ata_write_sectors(unsigned long start,
                      int count,
                      const void* buf)
{
    int ret = 0;
    int i;
    unsigned long addr;
    unsigned char response;
    tCardInfo *card = &card_info[current_card];

    if (start == 0)
        panicf("Writing on sector 0\n");

    addr = start * SECTOR_SIZE;

    mutex_lock(&ata_mtx);
    ret = select_card(current_card);

    for (i = 0; (i < count) && (ret == 0); i++)
    {
        if ((ret = send_cmd(CMD_WRITE_BLOCK, addr, &response)))
            break;
        ret = send_data(buf, SECTOR_SIZE, card->write_timeout);

        addr += SECTOR_SIZE;
        buf += SECTOR_SIZE;
    }

    deselect_card();
    mutex_unlock(&ata_mtx);

    return ret;
}

/* no need to delay with flash memory. There is no spinup :) */
extern void ata_delayed_write(unsigned long sector, const void* buf)
{
    ata_write_sectors(sector, 1, buf);
}

extern void ata_flush(void)
{
}

void ata_spindown(int seconds)
{
    (void)seconds;
}

bool ata_disk_is_active(void)
{
    return true;
}

int ata_standby(int time)
{
    int ret = 0;

    mutex_lock(&ata_mtx);

    /* ToDo: is there an equivalent? */
    (void)time;

    mutex_unlock(&ata_mtx);
    return ret;
}

int ata_sleep(void)
{
    return 0;
}

void ata_spin(void)
{
}

static void mmc_thread(void)
{
    struct event ev;

    while (1) {
        while ( queue_empty( &mmc_queue ) ) {

            sleep(HZ/4);
        }
        queue_wait(&mmc_queue, &ev);
        switch ( ev.id ) {
#ifndef USB_NONE
            case SYS_USB_CONNECTED:
                /* Tell the USB thread that we are safe */
                DEBUGF("mmc_thread got SYS_USB_CONNECTED\n");
                usb_acknowledge(SYS_USB_CONNECTED_ACK);

                /* Wait until the USB cable is extracted again */
                usb_wait_for_disconnect(&mmc_queue);
                break;
#endif
        }
    }
}

int ata_soft_reset(void)
{
    int ret = 0;

    return ret;
}

void ata_enable(bool on)
{
    PBCR1 &= ~0x0CF0; /* PB13, PB11 and PB10 become GPIOs, if not modified below */
    if (on)
    {
        PBCR1 |= 0x08A0;    /* as SCK1, TxD1, RxD1 */
        IPRE &= 0x0FFF;     /* disable SCI1 interrupts for the CPU */
    }
    and_b(~0x80, &PADRL); /* assert reset */
    sleep(HZ/20);
    or_b(0x80, &PADRL);   /* de-assert reset */
    sleep(HZ/20);
    card_info[0].initialized = false;
    card_info[1].initialized = false;
}

int ata_init(void)
{
    int rc = 0;

    mutex_init(&ata_mtx);

    led(false);

    /* Port setup */
    PADR |= 0x0680;   /* set all the selects + reset high (=inactive) */
    PAIOR |= 0x1680;  /* make outputs for them and the PA12 clock gate */

    PBDR |= 0x2C00;   /* SCK1, TxD1 and RxD1 high when GPIO  CHECKME: mask */
    PBIOR |= 0x2000;  /* SCK1 output */
    PBIOR &= ~0x0C00; /* TxD1, RxD1 input */

    if(adc_read(ADC_MMC_SWITCH) < 0x200)
    {   /* MMC inserted */
        current_card = 1;
    }
    else
    {   /* no MMC, use internal memory */
        current_card = 0;
    }

    ata_enable(true);

    if ( !initialized ) {

        queue_init(&mmc_queue);

        create_thread(mmc_thread, mmc_stack,
                      sizeof(mmc_stack), mmc_thread_name);
        initialized = true;
    }

    return rc;
}

#endif /* #ifdef HAVE_MMC */
