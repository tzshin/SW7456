/*
 * This file is part of SW7456.
 *
 * SW7456 is free software. You can redistribute this software and/or modify this software
 * under the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * SW7456 is distributed in the hope that they will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */
// *********************************************************************************************** //
/*
 * osd_spi.c
 *
 *  Created on: Sep 24, 2019
 *      Author: todd
 */
// *********************************************************************************************** //
#include "main.h"
#include "osd_spi.h"
#include "osd_registers.h"
// *********************************************************************************************** //
extern SPI_HandleTypeDef hspi1;
// *********************************************************************************************** //
U8 spi_data[SPI_BUFF_SIZE] ; // __attribute__ ((section (".spi_buffer")));
U8 spi_tx_response;
// *********************************************************************************************** //
// Call immediately after MX_SPI1_Init() — before anything else.
// Waits for NSS HIGH (BF has configured CS as output-high), then pre-loads TX FIFO.
// This prevents spurious SCK glitches during FC boot from consuming FIFO bytes.
void osd_spi_preload(void)
{
spi_tx_response=R_7456_OSDM_DEFAULT;

// Wait for NSS to go HIGH — BF configures OSD_CS as output-high before OSDM read.
// PA4 has internal pullup, but FC's PB12 floats low at reset.
// Timeout: ~100ms at 64MHz. If NSS never goes high, proceed anyway.
volatile U32 timeout=6400000;
while (!(GPIOA->IDR & (1<<4)) && --timeout) ;

SET_BIT(SPI1->CR1, SPI_CR1_SPE);
// Fill all 4 FIFO slots with 0x1B — no matter which byte BF captures, it's OSDM default
*(volatile uint8_t *)&SPI1->DR = spi_tx_response;
*(volatile uint8_t *)&SPI1->DR = spi_tx_response;
*(volatile uint8_t *)&SPI1->DR = spi_tx_response;
*(volatile uint8_t *)&SPI1->DR = spi_tx_response;
CLEAR_BIT(SPI1->CR2, SPI_CR2_ERRIE);
}

// Call during normal init — sets up DMA for RX processing.
void osd_spi_start(void)
{
HAL_SPI_Receive_DMA(&hspi1,(uint8_t *)spi_data,SPI_BUFF_SIZE);

// HAL re-enables ERRIE — disable again (OVR handler aborts DMA)
CLEAR_BIT(SPI1->CR2, SPI_CR2_ERRIE);
}
// *********************************************************************************************** //
void osd_spi_stop(void)
{
// unused
}
// *********************************************************************************************** //
void osd_spi_process(void)
{
static U32 x_last=0;
static U8 last_tx=R_7456_OSDM_DEFAULT;
U32 x;
U32 *CNDTR2=(U32 *)(0x0020 + ((int)DMA1));

x=(SPI_BUFF_SIZE-*CNDTR2)&SPI_BUFF_MASK;

while (x!=x_last)
	{
	osd_decode_spi_registers(spi_data[x_last]);
	x_last++; x_last&=SPI_BUFF_MASK;
	}

// Push new TX response when VM0 shadow changes (TXE = FIFO has room)
if (spi_tx_response!=last_tx && (SPI1->SR & SPI_SR_TXE))
	{
	*(volatile uint8_t *)&SPI1->DR = spi_tx_response;
	last_tx=spi_tx_response;
	}
}
// *********************************************************************************************** //
