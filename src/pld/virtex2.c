/***************************************************************************
 *   Copyright (C) 2006 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "virtex2.h"
#include "xilinx_bit.h"
#include "pld.h"


static int virtex2_set_instr(struct jtag_tap *tap, uint32_t new_instr)
{
	if (tap == NULL)
		return ERROR_FAIL;

	if (buf_get_u32(tap->cur_instr, 0, tap->ir_length) != new_instr)
	{
		struct scan_field field;

		field.tap = tap;
		field.num_bits = tap->ir_length;
		field.out_value = calloc(DIV_ROUND_UP(field.num_bits, 8), 1);
		buf_set_u32(field.out_value, 0, field.num_bits, new_instr);
		field.in_value = NULL;

		jtag_add_ir_scan(1, &field, jtag_set_end_state(TAP_IDLE));

		free(field.out_value);
	}

	return ERROR_OK;
}

static int virtex2_send_32(struct pld_device *pld_device,
		int num_words, uint32_t *words)
{
	struct virtex2_pld_device *virtex2_info = pld_device->driver_priv;
	struct scan_field scan_field;
	uint8_t *values;
	int i;

	values = malloc(num_words * 4);

	scan_field.tap = virtex2_info->tap;
	scan_field.num_bits = num_words * 32;
	scan_field.out_value = values;
	scan_field.in_value = NULL;

	for (i = 0; i < num_words; i++)
		buf_set_u32(values + 4 * i, 0, 32, flip_u32(*words++, 32));

	virtex2_set_instr(virtex2_info->tap, 0x5); /* CFG_IN */

	jtag_add_dr_scan(1, &scan_field, jtag_set_end_state(TAP_DRPAUSE));

	free(values);

	return ERROR_OK;
}

static __inline__ void virtexflip32(jtag_callback_data_t arg)
{
  uint8_t *in = (uint8_t *)arg;
	*((uint32_t *)in) = flip_u32(le_to_h_u32(in), 32);
}

static int virtex2_receive_32(struct pld_device *pld_device,
		int num_words, uint32_t *words)
{
	struct virtex2_pld_device *virtex2_info = pld_device->driver_priv;
	struct scan_field scan_field;

	scan_field.tap = virtex2_info->tap;
	scan_field.num_bits = 32;
	scan_field.out_value = NULL;
	scan_field.in_value = NULL;

	virtex2_set_instr(virtex2_info->tap, 0x4); /* CFG_OUT */

	while (num_words--)
	{
		scan_field.in_value = (uint8_t *)words;

		jtag_add_dr_scan(1, &scan_field, jtag_set_end_state(TAP_DRPAUSE));

		jtag_add_callback(virtexflip32, (jtag_callback_data_t)words);

		words++;;
	}

	return ERROR_OK;
}

static int virtex2_read_stat(struct pld_device *pld_device, uint32_t *status)
{
	uint32_t data[5];

	jtag_add_tlr();

	data[0] = 0xaa995566; /* synch word */
	data[1] = 0x2800E001; /* Type 1, read, address 7, 1 word */
	data[2] = 0x20000000; /* NOOP (Type 1, read, address 0, 0 words */
	data[3] = 0x20000000; /* NOOP */
	data[4] = 0x20000000; /* NOOP */
	virtex2_send_32(pld_device, 5, data);

	virtex2_receive_32(pld_device, 1, status);

	jtag_execute_queue();

	LOG_DEBUG("status: 0x%8.8" PRIx32 "", *status);

	return ERROR_OK;
}

static int virtex2_load(struct pld_device *pld_device, const char *filename)
{
	struct virtex2_pld_device *virtex2_info = pld_device->driver_priv;
	struct xilinx_bit_file bit_file;
	int retval;
	unsigned int i;
	struct scan_field field;

	field.tap = virtex2_info->tap;
	field.in_value = NULL;

	if ((retval = xilinx_read_bit_file(&bit_file, filename)) != ERROR_OK)
		return retval;

	jtag_set_end_state(TAP_IDLE);
	virtex2_set_instr(virtex2_info->tap, 0xb); /* JPROG_B */
	jtag_execute_queue();
	jtag_add_sleep(1000);

	virtex2_set_instr(virtex2_info->tap, 0x5); /* CFG_IN */
	jtag_execute_queue();

	for (i = 0; i < bit_file.length; i++)
		bit_file.data[i] = flip_u32(bit_file.data[i], 8);

	field.num_bits = bit_file.length * 8;
	field.out_value = bit_file.data;

	jtag_add_dr_scan(1, &field, jtag_set_end_state(TAP_DRPAUSE));
	jtag_execute_queue();

	jtag_add_tlr();

	jtag_set_end_state(TAP_IDLE);
	virtex2_set_instr(virtex2_info->tap, 0xc); /* JSTART */
	jtag_add_runtest(13, jtag_set_end_state(TAP_IDLE));
	virtex2_set_instr(virtex2_info->tap, 0x3f); /* BYPASS */
	virtex2_set_instr(virtex2_info->tap, 0x3f); /* BYPASS */
	virtex2_set_instr(virtex2_info->tap, 0xc); /* JSTART */
	jtag_add_runtest(13, jtag_set_end_state(TAP_IDLE));
	virtex2_set_instr(virtex2_info->tap, 0x3f); /* BYPASS */
	jtag_execute_queue();

	return ERROR_OK;
}

COMMAND_HANDLER(virtex2_handle_read_stat_command)
{
	struct pld_device *device;
	struct virtex2_pld_device *virtex2_info;
	uint32_t status;

	if (CMD_ARGC < 1)
	{
		command_print(cmd_ctx, "usage: virtex2 read_stat <num>");
		return ERROR_OK;
	}

	unsigned dev_id;
	COMMAND_PARSE_NUMBER(uint, args[0], dev_id);
	device = get_pld_device_by_num(dev_id);
	if (!device)
	{
		command_print(cmd_ctx, "pld device '#%s' is out of bounds", args[0]);
		return ERROR_OK;
	}

	virtex2_info = device->driver_priv;

	virtex2_read_stat(device, &status);

	command_print(cmd_ctx, "virtex2 status register: 0x%8.8" PRIx32 "", status);

	return ERROR_OK;
}

PLD_DEVICE_COMMAND_HANDLER(virtex2_pld_device_command)
{
	struct jtag_tap *tap;

	struct virtex2_pld_device *virtex2_info;

	if (CMD_ARGC < 2)
	{
		LOG_WARNING("incomplete pld device 'virtex2' configuration");
		return ERROR_PLD_DEVICE_INVALID;
	}

	tap = jtag_tap_by_string(args[1]);
	if (tap == NULL) {
		command_print(cmd_ctx, "Tap: %s does not exist", args[1]);
		return ERROR_OK;
	}

	virtex2_info = malloc(sizeof(struct virtex2_pld_device));
	virtex2_info->tap = tap;

	pld->driver_priv = virtex2_info;

	return ERROR_OK;
}

static int virtex2_register_commands(struct command_context *cmd_ctx)
{
	struct command *virtex2_cmd = register_command(cmd_ctx, NULL, "virtex2",
			NULL, COMMAND_ANY, "virtex2 specific commands");

	register_command(cmd_ctx, virtex2_cmd, "read_stat",
			&virtex2_handle_read_stat_command, COMMAND_EXEC,
			"read Virtex-II status register");

	return ERROR_OK;
}

struct pld_driver virtex2_pld = {
		.name = "virtex2",
		.register_commands = &virtex2_register_commands,
		.pld_device_command = &virtex2_pld_device_command,
		.load = &virtex2_load,
	};
