/*
 * sd_spi.c
 *
 * SD-over-SPI driver (ChaN mmc_spi reference, adapted to STM32 HAL).
 * SPI1 = hspi1 (PA5 SCK / PA6 MISO / PA7 MOSI), CS = PA4 (software NSS).
 *
 * Init runs at prescaler /256 (~328 kHz, required <400 kHz for the SD SPI
 * handshake); after the card is initialized the clock drops to /8 (~10.5 MHz)
 * for data transfer. Called from the user_diskio.c USER CODE blocks.
 */

#include "sd_spi.h"
#include "main.h"        /* hspi1, GPIO defines */
#include <string.h>

extern SPI_HandleTypeDef hspi1;

/* ---- MMC/SD command set ------------------------------------------------- */
#define CMD0    (0)         /* GO_IDLE_STATE */
#define CMD1    (1)         /* SEND_OP_COND (MMC) */
#define ACMD41  (0x80+41)   /* SEND_OP_COND (SDC) */
#define CMD8    (8)         /* SEND_IF_COND */
#define CMD9    (9)         /* SEND_CSD */
#define CMD12   (12)        /* STOP_TRANSMISSION */
#define CMD16   (16)        /* SET_BLOCKLEN */
#define CMD17   (17)        /* READ_SINGLE_BLOCK */
#define CMD18   (18)        /* READ_MULTIPLE_BLOCK */
#define ACMD23  (0x80+23)   /* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24   (24)        /* WRITE_BLOCK */
#define CMD25   (25)        /* WRITE_MULTIPLE_BLOCK */
#define CMD55   (55)        /* APP_CMD */
#define CMD58   (58)        /* READ_OCR */

/* Card type flags (CardType) */
#define CT_MMC      0x01
#define CT_SD1      0x02
#define CT_SD2      0x04
#define CT_SDC      (CT_SD1|CT_SD2)
#define CT_BLOCK    0x08

/* CS control on PA4. */
#define SD_CS_LOW()   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET)
#define SD_CS_HIGH()  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET)

static volatile DSTATUS Stat = STA_NOINIT;
static volatile UINT    Timer1, Timer2;   /* 1 kHz decrement timers (TIM6) */
static BYTE             CardType;

/* ---- Low-level SPI helpers --------------------------------------------- */

static BYTE xchg_spi(BYTE dat)
{
    BYTE rx = 0xFF;
    HAL_SPI_TransmitReceive(&hspi1, &dat, &rx, 1, 50);
    return rx;
}

static void rcvr_spi_multi(BYTE *buff, UINT btr)
{
    memset(buff, 0xFF, btr);
    HAL_SPI_TransmitReceive(&hspi1, buff, buff, btr, 1000);
}

static void xmit_spi_multi(const BYTE *buff, UINT btx)
{
    HAL_SPI_Transmit(&hspi1, (BYTE *)buff, btx, 1000);
}

/* Set SPI clock: slow (~328 kHz, prescaler 256) for init, fast for data.
 * Data clock lowered to /8 (~10.5 MHz) for tolerance with older/slower cards;
 * raise back to /4 once a known-good fast card is in use. */
static void FCLK_SLOW(void)
{
    hspi1.Instance->CR1 = (hspi1.Instance->CR1 & ~SPI_CR1_BR_Msk)
                        | SPI_BAUDRATEPRESCALER_256;
}
static void FCLK_FAST(void)
{
    hspi1.Instance->CR1 = (hspi1.Instance->CR1 & ~SPI_CR1_BR_Msk)
                        | SPI_BAUDRATEPRESCALER_8;
}

static int wait_ready(UINT wt)
{
    BYTE d;
    Timer2 = wt;
    do {
        d = xchg_spi(0xFF);
    } while (d != 0xFF && Timer2);
    return (d == 0xFF) ? 1 : 0;
}

static void deselect(void)
{
    SD_CS_HIGH();
    xchg_spi(0xFF);   /* >=8 idle clocks with CS high so the card releases DO */
    xchg_spi(0xFF);
}

static int select_card(void)
{
    SD_CS_LOW();
    xchg_spi(0xFF);
    if (wait_ready(500)) return 1;
    deselect();
    return 0;
}

static int rcvr_datablock(BYTE *buff, UINT btr)
{
    BYTE token;
    Timer1 = 200;
    do {
        token = xchg_spi(0xFF);
    } while ((token == 0xFF) && Timer1);
    if (token != 0xFE) return 0;        /* invalid data token */
    rcvr_spi_multi(buff, btr);
    xchg_spi(0xFF); xchg_spi(0xFF);     /* discard CRC */
    return 1;
}

static int xmit_datablock(const BYTE *buff, BYTE token)
{
    BYTE resp;
    if (!wait_ready(500)) return 0;
    xchg_spi(token);
    if (token != 0xFD) {                /* not StopTran */
        xmit_spi_multi(buff, 512);
        xchg_spi(0xFF); xchg_spi(0xFF); /* dummy CRC */
        resp = xchg_spi(0xFF);
        if ((resp & 0x1F) != 0x05) return 0;
    }
    return 1;
}

static BYTE send_cmd(BYTE cmd, DWORD arg)
{
    BYTE n, res;

    if (cmd & 0x80) {                   /* ACMD<n>: send CMD55 first */
        cmd &= 0x7F;
        res = send_cmd(CMD55, 0);
        if (res > 1) return res;
        /* CMD55 leaves CS asserted and the card still shifting out the tail of
         * its R1. Force a clean deselect + a full dummy byte so the card
         * re-syncs DO before the following ACMD; without this the ACMD41
         * response is read one bit early and comes back as 0x7F. */
        deselect();
    }

    if (cmd != CMD12) {
        deselect();
        if (!select_card()) return 0xFF;
    }

    xchg_spi(0x40 | cmd);
    xchg_spi((BYTE)(arg >> 24));
    xchg_spi((BYTE)(arg >> 16));
    xchg_spi((BYTE)(arg >> 8));
    xchg_spi((BYTE)arg);
    n = 0x01;
    if (cmd == CMD0) n = 0x95;
    if (cmd == CMD8) n = 0x87;
    xchg_spi(n);

    if (cmd == CMD12) xchg_spi(0xFF);   /* discard one byte after CMD12 */
    n = 10;
    do {
        res = xchg_spi(0xFF);
    } while ((res & 0x80) && --n);
    return res;
}

/* ---- FatFs disk-IO backends -------------------------------------------- */

DSTATUS SD_disk_initialize(void)
{
    BYTE n, cmd, ty, ocr[4];

    FCLK_SLOW();
    for (n = 10; n; n--) xchg_spi(0xFF);   /* >=74 dummy clocks, CS high */

    ty = 0;
    if (send_cmd(CMD0, 0) == 1) {          /* enter idle state */
        Timer1 = 2000;                     /* init timeout 2 s (old/slow card) */
        if (send_cmd(CMD8, 0x1AA) == 1) {  /* SDv2? */
            for (n = 0; n < 4; n++) ocr[n] = xchg_spi(0xFF);
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {      /* 2.7-3.6V */
                /* Poll ACMD41 at the spec-standard ~1 ms cadence. send_cmd()
                 * drives CMD55 internally (one ACMD = CMD55 + cmd). Pace with
                 * HAL_Delay (SdTask is task context): it does NOT touch
                 * Timer1/Timer2, which select_card()/wait_ready() reserve.
                 * Un-paced hammering over-runs a slow card and it stops
                 * responding (0xFF after a few valid 0x01 replies). */
                while (Timer1 && send_cmd(ACMD41, 0x40000000)) {
                    HAL_Delay(1);
                }
                if (Timer1 && send_cmd(CMD58, 0) == 0) {  /* read OCR */
                    for (n = 0; n < 4; n++) ocr[n] = xchg_spi(0xFF);
                    ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;  /* SDHC? */
                }
            }
        } else {                            /* SDv1 or MMCv3 */
            if (send_cmd(ACMD41, 0) <= 1) { cmd = ACMD41; ty = CT_SD1; }
            else                          { cmd = CMD1;   ty = CT_MMC; }
            while (Timer1 && send_cmd(cmd, 0)) ;
            if (!Timer1 || send_cmd(CMD16, 512) != 0) ty = 0;  /* set block len */
        }
    }
    CardType = ty;
    deselect();

    if (ty) {
        FCLK_FAST();
        Stat &= ~STA_NOINIT;
    } else {
        Stat = STA_NOINIT;
    }
    return Stat;
}

DSTATUS SD_disk_status(void)
{
    return Stat;
}

DRESULT SD_disk_read(BYTE *buff, DWORD sector, UINT count)
{
    if (!count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    if (!(CardType & CT_BLOCK)) sector *= 512;  /* byte addressing (non-SDHC) */

    if (count == 1) {
        if ((send_cmd(CMD17, sector) == 0) && rcvr_datablock(buff, 512))
            count = 0;
    } else {
        if (send_cmd(CMD18, sector) == 0) {     /* READ_MULTIPLE_BLOCK */
            do {
                if (!rcvr_datablock(buff, 512)) break;
                buff += 512;
            } while (--count);
            send_cmd(CMD12, 0);                 /* STOP_TRANSMISSION */
        }
    }
    deselect();
    return count ? RES_ERROR : RES_OK;
}

DRESULT SD_disk_write(const BYTE *buff, DWORD sector, UINT count)
{
    if (!count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    if (!(CardType & CT_BLOCK)) sector *= 512;  /* byte addressing (non-SDHC) */

    if (count == 1) {
        if ((send_cmd(CMD24, sector) == 0) && xmit_datablock(buff, 0xFE))
            count = 0;
    } else {
        if (CardType & CT_SDC) send_cmd(ACMD23, count);
        if (send_cmd(CMD25, sector) == 0) {     /* WRITE_MULTIPLE_BLOCK */
            do {
                if (!xmit_datablock(buff, 0xFC)) break;
                buff += 512;
            } while (--count);
            if (!xmit_datablock(0, 0xFD)) count = 1;  /* StopTran token */
        }
    }
    deselect();
    return count ? RES_ERROR : RES_OK;
}

DRESULT SD_disk_ioctl(BYTE cmd, void *buff)
{
    DRESULT res = RES_ERROR;
    BYTE n, csd[16];
    DWORD csize;

    if (Stat & STA_NOINIT) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        if (select_card()) res = RES_OK;
        deselect();
        break;

    case GET_SECTOR_COUNT:
        if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
            if ((csd[0] >> 6) == 1) {       /* SDC v2 */
                csize = csd[9] + ((WORD)csd[8] << 8)
                      + ((DWORD)(csd[7] & 63) << 16) + 1;
                *(DWORD *)buff = csize << 10;
            } else {                        /* SDC v1 / MMC */
                n = (csd[5] & 15) + ((csd[10] & 128) >> 7)
                  + ((csd[9] & 3) << 1) + 2;
                csize = (csd[8] >> 6) + ((WORD)csd[7] << 2)
                      + ((WORD)(csd[6] & 3) << 10) + 1;
                *(DWORD *)buff = csize << (n - 9);
            }
            res = RES_OK;
        }
        deselect();
        break;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 128;
        res = RES_OK;
        break;

    default:
        res = RES_PARERR;
        break;
    }
    return res;
}

/* 1 ms timing tick - call from the TIM6 (1 kHz) period-elapsed callback. */
void SD_TimerProc(void)
{
    UINT t;
    t = Timer1; if (t) Timer1 = t - 1;
    t = Timer2; if (t) Timer2 = t - 1;
}
