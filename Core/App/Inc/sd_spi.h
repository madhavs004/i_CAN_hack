/*
 * sd_spi.h
 *
 * SD-card-over-SPI low-level driver (ChaN mmc_spi reference, adapted to STM32
 * HAL). Bound to hspi1 (PA5 SCK / PA6 MISO / PA7 MOSI), CS on PA4.
 *
 * These functions implement the FatFs disk-IO contract and are called from the
 * thin USER CODE blocks in FATFS/Target/user_diskio.c. They run in task context
 * (FatFs is reentrant / CMSIS-guarded). NOT ISR-safe.
 *
 * SD_TimerProc() is the ONLY function safe to call from an ISR; wire it to the
 * 1 kHz TIM6 period-elapsed callback so the driver's internal timeouts work.
 */

#ifndef APP_INC_SD_SPI_H_
#define APP_INC_SD_SPI_H_

#include "diskio.h"   /* DSTATUS / DRESULT / BYTE / DWORD / UINT */

/* FatFs disk-IO backends (called from user_diskio.c USER CODE blocks). */
DSTATUS SD_disk_initialize(void);
DSTATUS SD_disk_status(void);
DRESULT SD_disk_read(BYTE *buff, DWORD sector, UINT count);
DRESULT SD_disk_write(const BYTE *buff, DWORD sector, UINT count);
DRESULT SD_disk_ioctl(BYTE cmd, void *buff);

/* 1 ms timing tick - call from the TIM6 (1 kHz) period-elapsed callback. */
void SD_TimerProc(void);

#endif /* APP_INC_SD_SPI_H_ */
