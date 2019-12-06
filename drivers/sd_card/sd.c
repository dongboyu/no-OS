/***************************************************************************//**
*   @file   sd.c
*   @brief  Implementation of SD card interface over SPI.
*   This interface supports byte read and write operations for SD cards that
*   that meet the following conditions:
*   	- Version 2.00 or later
*   	- High capacity or extended capacity (SDHX or SDXC)
*   	- Supply voltage of 3.3V
*   @author Mihail Chindris (mihail.chindris@analog.com)
********************************************************************************
* Copyright 2019(c) Analog Devices, Inc.
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*  - Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
*  - Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in
*    the documentation and/or other materials provided with the
*    distribution.
*  - Neither the name of Analog Devices, Inc. nor the names of its
*    contributors may be used to endorse or promote products derived
*    from this software without specific prior written permission.
*  - The use of this software may or may not infringe the patent rights
*    of one or more patent holders.  This license does not release you
*    from the requirement that you obtain separate licenses from these
*    patent holders to use this software.
*  - Use of the software either in source or binary form, must be run
*    on or directly connected to an Analog Devices Inc. component.
*
* THIS SOFTWARE IS PROVIDED BY ANALOG DEVICES "AS IS" AND ANY EXPRESS OR
* IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, NON-INFRINGEMENT,
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL ANALOG DEVICES BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, INTELLECTUAL PROPERTY RIGHTS, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

#include "sd.h"

/**
 * Read SD card bytes until one is different from 0xFF
 * @param sd_desc	- Instance of the SD card
 * @param data_out	- The read bytes is wrote here
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
static uint32_t wait_for_response(struct sd_desc *sd_desc, uint8_t *data_out)
{
	*data_out = 0xFF;
	while (*data_out == 0xFF)
		if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, data_out, 1))
			return FAILURE;
	return SUCCESS;
}

/**
 * Read SD card bytes until one is different from 0x00
 * @param sd_desc - Instance of the SD card
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
static uint32_t wait_until_not_busy(struct sd_desc *sd_desc){
	uint8_t data;

	data = 0x00;
	while (data == 0x00){
		data = 0xFF;
		if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, &data, 1))
			return FAILURE;
	}

	return SUCCESS;
}

/**
 * Build command with cmd_desc, send it to the SD card and write the
 * response in the response field of cmd_desc
 * @param sd_desc	- Instance of the SD card
 * @param cmd_desc	- Structure with needed fields to build the command
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
static uint32_t send_command(struct sd_desc *sd_desc, struct cmd_desc *cmd_desc)
{
	/* Send CMD55 if it is an application command */
	if (cmd_desc->cmd & BIT_APPLICATION_CMD) {
		struct cmd_desc	cmd_desc_local;
		cmd_desc_local.cmd = CMD(55);
		cmd_desc_local.arg = STUFF_ARG;
		cmd_desc_local.response_len = R1_LEN;
		if (SUCCESS != send_command(sd_desc, &cmd_desc_local))
			return FAILURE;
		if (cmd_desc_local.response[0] != R1_IDLE_STATE){
			DEBUG_MSG("Not the expected response for CMD55\n");
			return FAILURE;
		}
	}

	/* Prepare command in buffer */
	memset((uint8_t *)sd_desc->buff, 0xFF, CMD_LEN);
	sd_desc->buff[1] = cmd_desc->cmd & (~BIT_APPLICATION_CMD);	/* Set cmd */
	sd_desc->buff[2] = (cmd_desc->arg >> 24) & 0xff;			/* Set argument */
	sd_desc->buff[3] = (cmd_desc->arg >> 16) & 0xff;
	sd_desc->buff[4] = (cmd_desc->arg >> 8) & 0xff;
	sd_desc->buff[5] = cmd_desc->arg & 0xff;
	if (cmd_desc->cmd == CMD(0))								/* Set crc if needed */
		sd_desc->buff[6] = 0x95;
	else if (cmd_desc->cmd == CMD(8))
		sd_desc->buff[6] = 0x87;

	/* Send command */
	if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, sd_desc->buff, CMD_LEN))
		return FAILURE;

	/* Read response */
	if (SUCCESS != wait_for_response(sd_desc, cmd_desc->response))
		return FAILURE;
	if (cmd_desc->response_len - 1 > 0){
		memset(cmd_desc->response + 1, 0xFF, cmd_desc->response_len - 1);
		if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, cmd_desc->response + 1, cmd_desc->response_len - 1))
			return FAILURE;
	}

	return SUCCESS;
}

/**
 * Send one block of data to the SD card
 * @param sd_desc		- Instance of the SD card
 * @param data			- Data to be written
 * @param nb_of_blocks	- Number of blocks written in the executing command
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
static uint32_t write_block(struct sd_desc *sd_desc, uint8_t *data, uint32_t nb_of_blocks)
{
	/* Send start block token */
	sd_desc->buff[0] = START_N_BLOCK_TOKEN;
	if (nb_of_blocks == 1)
		sd_desc->buff[0] = START_1_BLOCK_TOKEN;
	if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, sd_desc->buff, 1))
			return FAILURE;

	/* Send data with CRC */
	if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, data, DATA_BLOCK_LEN))
			return FAILURE;
	*((uint16_t *)sd_desc->buff) = 0xFFFF;
	if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, sd_desc->buff, CRC_LEN))
			return FAILURE;

	/* Read response and check if write was ok */
	uint8_t		response;
	if (SUCCESS != wait_for_response(sd_desc, &response))
		return FAILURE;
	switch (response & MASK_RESPONSE_TOKEN)
	{
	case 0x4: break;
	case 0xA: DEBUG_MSG("CRC error\n"); return FAILURE;
	case 0xC: DEBUG_MSG("Write error\n"); return FAILURE;
	default: DEBUG_MSG("Other problem\n"); return FAILURE;
	}
	if (SUCCESS != wait_until_not_busy(sd_desc))
		return FAILURE;

	return SUCCESS;
}

/**
 * Read one block of data to the SD card
 * @param sd_desc	- Instance of the SD card
 * @param data		- Buffer were data will be read
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
static uint32_t read_block(struct sd_desc *sd_desc, uint8_t *data)
{
	/* Reading Start block token */
	uint8_t		response;
	if (SUCCESS != wait_for_response(sd_desc, &response))
		return FAILURE;
	if ((response & MASK_ERROR_TOKEN) == 0) {
		DEBUG_MSG("Received data error token on read\n");
		switch (response) {
		case 0x1: DEBUG_MSG("Error\n"); break;
		case 0x2: DEBUG_MSG("CC Error\n"); break;
		case 0x4: DEBUG_MSG("Card ECC Failed\n"); break;
		case 0x8: DEBUG_MSG("Out of range\n"); break;
		default: DEBUG_MSG("Multiple errors\n"); break;
		}
		return FAILURE;
	}
	if (response != START_1_BLOCK_TOKEN) {
		DEBUG_MSG("Not expected response. Expecting start block token\n");
		return FAILURE;
	}

	/* Read data block */
	memset(data, 0xff, DATA_BLOCK_LEN);
	if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, data, DATA_BLOCK_LEN))
		return FAILURE;

	/* Read crc*/
	*((uint16_t *)sd_desc->buff) = 0xFFFF;
	if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, sd_desc->buff, CRC_LEN))
		return FAILURE;

	return SUCCESS;
}

/**
 * Prepare and write data block by block
 * @param sd_desc		- Instance of the SD card
 * @param data			- Data to be written
 * @param addr			- Address in memory where data will be written
 * @param len			- Length of data to be written
 * @param nb_of_blocks	- Number of block to be written
 * @param first_block	- First block where data will be written with unmodified
 * 						  data
 * @param last_block	- First block where data will be written with unmodified
 * 						  data
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
static uint32_t write_multiple_blocks(struct sd_desc *sd_desc,
			uint8_t *data, uint64_t addr, uint64_t len,
			uint32_t nb_of_blocks, uint8_t *first_block, uint8_t *last_block)
{
	uint16_t		buff_first_idx;
	uint16_t		buff_copy_len;
	uint32_t		i;
	uint64_t		data_idx;

	data_idx = 0;
	i = 0;
 	while (i < nb_of_blocks) {
		buff_first_idx = 0x0000u;
		if (i == 0)
			buff_first_idx = addr & MASK_ADDR_IN_BLOCK;
		buff_copy_len = DATA_BLOCK_LEN - buff_first_idx;
		if (i == nb_of_blocks - 1)
			buff_copy_len = ((addr + len - 1) & MASK_ADDR_IN_BLOCK) - buff_first_idx + 1;
		if (buff_first_idx == 0x0000u && buff_copy_len == DATA_BLOCK_LEN) {
			/* Write every block beside the first and last if the write is not the entire block */
			if (SUCCESS != write_block(sd_desc, data + data_idx, nb_of_blocks))
				return FAILURE;
		} else {							/* If we are not writing a full block */
			if (i == 0) {					/* If is the first block */
				memcpy(first_block + buff_first_idx, data + data_idx, buff_copy_len);
				if (SUCCESS != write_block(sd_desc, first_block, nb_of_blocks))
					return FAILURE;
			} else {						/* Is the last block */
				memcpy(last_block, data + data_idx, buff_copy_len);
				if (SUCCESS != write_block(sd_desc, last_block, nb_of_blocks))
					return FAILURE;
			}
		}
		data_idx += buff_copy_len;
		i++;
	}

	return SUCCESS;
}

/**
 * Prepare and read data block by block
 * @param sd_desc		- Instance of the SD card
 * @param data			- Data to be read
 * @param addr			- Address in memory from where data will be read
 * @param len			- Length of data to be read
 * @param nb_of_blocks	- Number of block to be read
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
static uint32_t read_multiple_blocks(struct sd_desc *sd_desc,
									uint8_t *data, uint64_t addr, uint64_t len,
									uint32_t nb_of_blocks)
{
	uint8_t			buff[DATA_BLOCK_LEN];
	uint32_t		i;
	uint64_t		data_idx;

	data_idx = 0;
	i = 0;
 	while (i < nb_of_blocks) {
 		uint16_t		buff_first_idx;
 		uint16_t		buff_copy_len;

		buff_first_idx = 0x0000u;
		if (i == 0)
			buff_first_idx = addr & MASK_ADDR_IN_BLOCK;
		buff_copy_len = DATA_BLOCK_LEN - buff_first_idx;
		if (i == nb_of_blocks - 1)
			buff_copy_len = ((addr + len - 1) & MASK_ADDR_IN_BLOCK) - buff_first_idx + 1;
		if (buff_first_idx == 0x0000u && buff_copy_len == DATA_BLOCK_LEN) {
			if (SUCCESS != read_block(sd_desc, data + data_idx))
				return FAILURE;
		} else {

			if (SUCCESS != read_block(sd_desc, buff))
				return FAILURE;
			memcpy(data + data_idx, buff + buff_first_idx, buff_copy_len);
		}
		data_idx += buff_copy_len;
		i++;
	}

	return SUCCESS;
}

/**
 * Read data of size len from the specified address and store it in data.
 * This operation returns only when the read is complete
 * @param sd_desc		- Instance of the SD card
 * @param data			- Where data will be read
 * @param addr			- Address in memory from where data will be read
 * @param len			- Length in bytes of data to be read
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
uint32_t sd_read(struct sd_desc *sd_desc,
						uint8_t *data, uint64_t address, uint64_t len)
{
	struct cmd_desc	cmd_desc;
	uint32_t		nb_of_blocks;
	uint32_t		first_block_addr;

	/* Initial checks */
	if (data == NULL || address > sd_desc->memory_size ||
			len > sd_desc->memory_size || address + len > sd_desc->memory_size)
		return FAILURE;

	/* Send read command */
	first_block_addr = address >> DATA_BLOCK_BITS;
	nb_of_blocks = ((address + len - 1) >> DATA_BLOCK_BITS) - first_block_addr + 1;

	cmd_desc.cmd = (nb_of_blocks == 1) ? CMD(17): CMD(18);
	cmd_desc.arg = first_block_addr;
	cmd_desc.response_len = R1_LEN;
	if (SUCCESS != send_command(sd_desc, &cmd_desc))
		return FAILURE;
	if (cmd_desc.response[0] != R1_READY_STATE) {
		DEBUG_MSG("Failed to write Data command\n");
		return FAILURE;
	}

	/* Read blocks */
	if (SUCCESS != read_multiple_blocks(sd_desc, data, address, len, nb_of_blocks))
		return FAILURE;

	/* Send stop transmission command */
	if (nb_of_blocks != 1){
		cmd_desc.cmd = CMD(12);
		cmd_desc.arg = STUFF_ARG;
		cmd_desc.response_len = R1_LEN;
		if (SUCCESS != send_command(sd_desc, &cmd_desc))
			return FAILURE;
		if(cmd_desc.response[0] != R1_READY_STATE) {
			DEBUG_MSG("Failed to send stop transmission command\n");
			return FAILURE;
		}
		if (SUCCESS != wait_until_not_busy(sd_desc))
			return FAILURE;
	}

	return SUCCESS;
}

/**
 * Write data of size len to the specified address
 * This operation returns only when the write is complete
 * @param sd_desc		- Instance of the SD card
 * @param data			- Data to write
 * @param addr			- Address in memory where data will be written
 * @param len			- Length of data in bytes
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
uint32_t sd_write(struct sd_desc *sd_desc, uint8_t *data, uint64_t address, uint64_t len)
{
	struct cmd_desc	cmd_desc;
	uint32_t		nb_of_blocks;
	uint32_t		first_block_addr;
	uint8_t			first_block[DATA_BLOCK_LEN];
	uint8_t			last_block[DATA_BLOCK_LEN];

	/* Initial checks */
	if (data == NULL || address > sd_desc->memory_size ||
			len > sd_desc->memory_size || address + len > sd_desc->memory_size)
		return FAILURE;

	/* Read first and last block if needed to be modified in memory with new data and then write them */
	if ((address & MASK_ADDR_IN_BLOCK) != 0 || /* If not writing from the beginning of a block or */
		((address & MASK_ADDR_IN_BLOCK) == 0 && len < DATA_BLOCK_LEN)) /* If writing from the beginning but not the full block */
		sd_read(sd_desc, first_block, address & MASK_BLOCK_NUMBER, DATA_BLOCK_LEN);
	if (((address + len - 1) & MASK_BLOCK_NUMBER) != (address & MASK_BLOCK_NUMBER) && /* If the last block is different from the first and */
			((address + len - 1) & MASK_ADDR_IN_BLOCK) != MASK_ADDR_IN_BLOCK) /* If reading less than the full block */
		sd_read(sd_desc, last_block, (address + len - 1) & MASK_BLOCK_NUMBER, DATA_BLOCK_LEN);

	/* Send write command to SD */
	first_block_addr = address >> DATA_BLOCK_BITS;
	nb_of_blocks = ((address + len - 1) >> DATA_BLOCK_BITS) - first_block_addr + 1;
	cmd_desc.cmd = (nb_of_blocks == 1) ? CMD(24): CMD(25);
	cmd_desc.arg = first_block_addr;
	cmd_desc.response_len = R1_LEN;
	if (SUCCESS != send_command(sd_desc, &cmd_desc))
		return FAILURE;
	if (cmd_desc.response[0] != R1_READY_STATE) {
		DEBUG_MSG("Failed to write Data command\n");
		return FAILURE;
	}

	/* Write blocks */
	if (SUCCESS != write_multiple_blocks(sd_desc, data, address, len, nb_of_blocks, first_block, last_block))
		return FAILURE;

	/* Send stop transmission token */
	if (nb_of_blocks != 1){
		sd_desc->buff[0] = STOP_TRANSMISSION_TOKEN;
		if (SUCCESS != spi_write_and_read(sd_desc->spi_desc, sd_desc->buff, 1))
				return FAILURE;
		if (SUCCESS != wait_until_not_busy(sd_desc))
			return FAILURE;
	}

	return SUCCESS;
}

/**
 * Initialize an instance of SD card and stores it to the parameter desc
 * @param sd_desc		- Pointer where to store the instance of the SD card
 * @param param			- Contains an initialized SPI descriptor
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
uint32_t sd_init(struct sd_desc **sd_desc, const struct sd_init_param *param)
{
	/* Allocate data and initialize sd_desc with param */
	if (!sd_desc || !param)
		return FAILURE;
	*sd_desc = calloc(1, sizeof(struct sd_desc));
	if (!(*sd_desc))
		return FAILURE;
	(*sd_desc)->spi_desc = param->spi_desc;

	struct sd_desc	*local_desc;
	struct cmd_desc	cmd_desc;
	uint32_t		i;

	/* Synchronize SD card frequency: Send 10 dummy bytes*/
	local_desc = *sd_desc;
	memset(local_desc->buff, 0xFF, 10);
	if (SUCCESS != spi_write_and_read(local_desc->spi_desc, local_desc->buff, 10))
		return FAILURE;

	/* Change from SD mode to SPI mode */
	cmd_desc.cmd = CMD(0);
	cmd_desc.arg = STUFF_ARG;
	cmd_desc.response_len = R1_LEN;
	i = 0;
	while (true) {
		if (SUCCESS != send_command(local_desc, &cmd_desc))
			return FAILURE;
		if (cmd_desc.response[0] == R1_IDLE_STATE)
			break;
		if (++i == CMD0_RETRY_NUMBER){
			DEBUG_MSG("Failed to enter SPI_MODE\n");
			return FAILURE;
		}
	}

	/* Check if SD is version 2.0 or later and its Voltage range*/
	cmd_desc.cmd = CMD(8);
	cmd_desc.arg = CMD8_ARG;
	cmd_desc.response_len = R3_LEN;
	if (SUCCESS != send_command(local_desc, &cmd_desc))
		return FAILURE;
	if (!(cmd_desc.response[0] == R1_IDLE_STATE &&
			cmd_desc.response[3] == 0x1u && cmd_desc.response[4] == 0xAAu)) {
		DEBUG_MSG("SD card is lower than V2.0 or not supported voltage\n");
		return FAILURE;
	}

	/* For enabling CRC send CMD 59 here (CRC not implemented)*/

	/* Change to ready state */
	cmd_desc.cmd = ACMD(41);
	cmd_desc.arg = ACMD41_ARG;
	cmd_desc.response_len = R1_LEN;
	while (true){
		if (SUCCESS != send_command(local_desc, &cmd_desc))
			return FAILURE;
		if (cmd_desc.response[0] == R1_READY_STATE)
			break;
		cmd_desc.arg = 0x00000000u;
	}

	/* Check if card is HC or XC (implemented only for this types) */
	cmd_desc.cmd = CMD(58);
	cmd_desc.arg = STUFF_ARG;
	cmd_desc.response_len = R3_LEN;
	if (SUCCESS != send_command(local_desc, &cmd_desc))
		return FAILURE;
	if (!(cmd_desc.response[0] == R1_READY_STATE && (cmd_desc.response[1] & (BIT_CCS >> 24)))) {
		DEBUG_MSG("Only SDHX and SDXC supported\n");
		return FAILURE;
	}

	/* Read CSD register to get memory size */
	cmd_desc.cmd = CMD(9);
	cmd_desc.arg = STUFF_ARG;
	cmd_desc.response_len = R1_LEN;
	if (SUCCESS != send_command(local_desc, &cmd_desc))
		return FAILURE;
	if (SUCCESS != wait_for_response(local_desc, cmd_desc.response))
			return FAILURE;
	if (cmd_desc.response[0] != START_1_BLOCK_TOKEN){
		DEBUG_MSG("Failed to read CSD register\n");
		return FAILURE;
	}
	memset(local_desc->buff, 0xFF, CSD_LEN);
	if (SUCCESS != spi_write_and_read(local_desc->spi_desc, local_desc->buff, CSD_LEN))
		return FAILURE;

	/* Get c_size from CSD */
	uint32_t c_size = ((local_desc->buff[7] & ((1u<<5) -1)) << 16) |
			(local_desc->buff[8] << 8) |
			local_desc->buff[9];
	local_desc->memory_size = ((uint64_t)c_size + 1) * ((uint64_t)DATA_BLOCK_LEN << 10u);
	return SUCCESS;
}

/**
 * Remove the initialize instance of SD card.
 * @param sd_desc		- Instance of the SD card
 * @return SUCCESS in case of success, FAILURE otherwise.
 */
uint32_t sd_remove(struct sd_desc *desc){
	if (desc == NULL)
		return FAILURE;
	free(desc);
	return SUCCESS;
}
