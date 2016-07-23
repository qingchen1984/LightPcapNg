// light_pcapng.c
// Created on: Jul 23, 2016

// Copyright (c) 2016 Radu Velea

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "light_pcapng.h"

#include "light_debug.h"
#include "light_internal.h"
#include "light_util.h"

#include <stdlib.h>
#include <string.h>

// Documentation from: https://github.com/pcapng/pcapng

static struct _light_option *__parse_options(uint32_t **memory, const ssize_t max_len)
{
	if (max_len <= 0) {
		return NULL;
	}
	else {
		struct _light_option *opt = calloc(1, sizeof(struct _light_option));
		uint16_t actual_length;
		uint16_t allignment = sizeof(uint32_t);

		uint16_t *local_memory = (uint16_t*)*memory;
		uint16_t remaining_size;

		opt->custom_option_code = *local_memory++;
		opt->option_length = *local_memory++;

		actual_length = (opt->option_length % allignment) == 0 ?
				opt->option_length :
				(opt->option_length / allignment + 1) * allignment;

		if (actual_length > 0) {
			opt->data = calloc(1, actual_length);
			memcpy(opt->data, local_memory, actual_length);
			local_memory += (sizeof(**memory) / sizeof(*local_memory)) * (actual_length / allignment);
		}

		*memory = (uint32_t*)local_memory;
		remaining_size = max_len - actual_length - 2 * sizeof(*local_memory);

		if (opt->custom_option_code == 0) {
			DCHECK_ASSERT(opt->option_length, 0, light_stop);
			DCHECK_ASSERT(remaining_size, 0, light_stop);

			if (remaining_size) {
				// XXX: Treat the remaining data as garbage and discard it form the trace.
				*memory += remaining_size / sizeof(uint32_t);
			}
		}
		else {
			opt->next_option = __parse_options(memory, remaining_size);
		}

		return opt;
	}
}

// Parse memory and allocate _light_pcapng array.
static void __parse_mem_copy(struct _light_pcapng **iter, const uint32_t *memory, const size_t size)
{
	const uint32_t *local_data = (const uint32_t *)(memory);
	struct _light_pcapng *current = NULL;
	size_t bytes_read = 0;

	if (size < 12)
		return;

	current = calloc(1, sizeof(struct _light_pcapng));
	current->block_type = *local_data++;
	current->block_total_lenght = *local_data++;
	DCHECK_INT(((current->block_total_lenght % 4) == 0), 0, light_stop);

	switch (current->block_type)
	{
	case LIGHT_SECTION_HEADER_BLOCK:
	{
		DPRINT_HERE(LIGHT_SECTION_HEADER_BLOCK);
		struct _light_section_header *shb = calloc(1, sizeof(struct _light_section_header));
		struct _light_option *opt = NULL;
		uint32_t version;
		ssize_t local_offset;

		shb->byteorder_magic = *local_data++;
		// TODO check byte order.
		version = *local_data++;
		shb->major_version = version & 0xFFFF;
		shb->minor_version = (version >> 16) & 0xFFFF;
		shb->section_length = *((uint64_t*)local_data);
		local_data += 2;

		current->block_body = (uint32_t*)shb;
		local_offset = (size_t)local_data - (size_t)memory;
		opt = __parse_options((uint32_t **)&local_data, current->block_total_lenght - local_offset - sizeof(current->block_total_lenght));
		current->options = opt;
	}
	break;

	case LIGHT_INTERFACE_BLOCK:
	{
		DPRINT_HERE(LIGHT_INTERFACE_BLOCK);
		struct _light_interface_description_block *idb = calloc(1, sizeof(struct _light_interface_description_block));
		struct _light_option *opt = NULL;
		uint32_t link_reserved = *local_data++;
		ssize_t local_offset;

		idb->link_type = link_reserved & 0xFFFF;
		idb->reserved = (link_reserved >> 16) & 0xFFFF;
		idb->snapshot_length = *local_data++;
		current->block_body = (uint32_t*)idb;
		local_offset = (size_t)local_data - (size_t)memory;
		opt = __parse_options((uint32_t **)&local_data, current->block_total_lenght - local_offset - sizeof(current->block_total_lenght));
		current->options = opt;
	}
	break;

	case LIGHT_ENHANCED_PACKET_BLOCK:
	{
		DPRINT_HERE(LIGHT_ENHANCED_PACKET_BLOCK);
		struct _light_enhanced_packet_block *epb = NULL;
		struct _light_option *opt = NULL;
		uint32_t interface_id = *local_data++;
		uint32_t timestamp_high = *local_data++;
		uint32_t timestamp_low = *local_data++;
		uint32_t captured_packet_length = *local_data++;
		uint32_t original_packet_length = *local_data++;
		ssize_t local_offset;
		uint32_t actual_len = 0;

		PADD32(captured_packet_length, &actual_len);

		epb = calloc(1, sizeof(struct _light_enhanced_packet_block) + actual_len);
		epb->interface_id = interface_id;
		epb->timestamp_high = timestamp_high;
		epb->timestamp_low = timestamp_low;
		epb->capture_packet_length = captured_packet_length;
		epb->original_capture_length = original_packet_length;

		memcpy(epb->packet_data, local_data, captured_packet_length); // Maybe actual_len?
		local_data += actual_len / sizeof(uint32_t);
		current->block_body = (uint32_t*)epb;
		local_offset = (size_t)local_data - (size_t)memory;
		opt = __parse_options((uint32_t **)&local_data, current->block_total_lenght - local_offset - sizeof(current->block_total_lenght));
		current->options = opt;
	}
	break;

	case LIGHT_SIMPLE_PACKET_BLOCK:
	{
		DPRINT_HERE(LIGHT_SIMPLE_PACKET_BLOCK);
		struct _light_simple_packet_block *spb = NULL;
		uint32_t original_packet_length = *local_data++;
		uint32_t actual_len = current->block_total_lenght - 2 * sizeof(current->block_total_lenght) - sizeof(current->block_type) - sizeof(original_packet_length);

		spb = calloc(1, sizeof(struct _light_enhanced_packet_block) + actual_len);
		spb->original_packet_length = original_packet_length;

		memcpy(spb->packet_data, local_data, actual_len);
		local_data += actual_len / sizeof(uint32_t);
		current->block_body = (uint32_t*)spb;
		current->options = NULL; // No options defined by the standard for this block type.
	}
	break;

	case LIGHT_CUSTOM_DATA_BLOCK:
	{
		DPRINT_HERE(LIGHT_CUSTOM_DATA_BLOCK);
		struct _light_custom_nonstandard_block *cnb = NULL;
		struct _light_option *opt = NULL;
		uint32_t len = *local_data++;
		uint32_t reserved0 = *local_data++;
		uint32_t reserved1 = *local_data++;
		ssize_t local_offset;
		uint32_t actual_len = 0;

		PADD32(len, &actual_len);
		cnb = calloc(1, sizeof(struct _light_custom_nonstandard_block) + actual_len);
		cnb->data_length = len;
		cnb->reserved0 = reserved0;
		cnb->reserved1 = reserved1;

		memcpy(cnb->packet_data, local_data, len); // Maybe actual_len?
		local_data += actual_len / sizeof(uint32_t);
		current->block_body = (uint32_t*)cnb;
		local_offset = (size_t)local_data - (size_t)memory;
		opt = __parse_options((uint32_t **)&local_data, current->block_total_lenght - local_offset - sizeof(current->block_total_lenght));
		current->options = opt;
	}
	break;

	default: // Could not find registered block type. Copying data as RAW.
	{
		DPRINT_HERE(default);
		uint32_t raw_size = current->block_total_lenght - 2 * sizeof(current->block_total_lenght) - sizeof(current->block_type);
		if (raw_size > 0) {
			current->block_body = calloc(raw_size, 1);
			memcpy(current->block_body, local_data, raw_size);
			local_data += raw_size / (sizeof(*local_data));
		}
		else {
			current->block_body = NULL;
		}
	}
	break;
	}

	// Compute offset and return new link.
	// Block total length.
	DCHECK_ASSERT((bytes_read = *local_data++), current->block_total_lenght, light_stop);

	current->next_block = NULL;
	bytes_read = current->block_total_lenght;

	__parse_mem_copy(&current->next_block, memory + (bytes_read / sizeof(*memory)), size - bytes_read);

	*iter = current;

	return;
}

light_pcapng light_read_from_memory(const uint32_t *memory, size_t size)
{
	struct _light_pcapng *head = NULL;
	__parse_mem_copy(&head, memory, size);
	return head;
}

static void __free_option(struct _light_option *option)
{
	if (option == NULL)
		return;

	__free_option(option->next_option);

	option->next_option = NULL;
	free(option->data);
	free(option);
}

void light_pcapng_release(light_pcapng pcapng)
{
	if (pcapng == NULL)
		return;

	light_pcapng_release(pcapng->next_block);
	pcapng->next_block = NULL;

	__free_option(pcapng->options);
	free(pcapng->block_body);
	free(pcapng);
}

static int __option_count(struct _light_option *option)
{
	if (option == NULL)
		return 0;

	return 1 + __option_count(option->next_option);
}

char *light_pcapng_to_string(light_pcapng pcapng)
{
	if (pcapng == NULL)
		return NULL;

	char *string;
	char *next = light_pcapng_to_string(pcapng->next_block);
	size_t buffer_size = 0;

	if (next != NULL)
		buffer_size += strlen(next);

	string = calloc(256 + buffer_size, 1);
	sprintf(string, "---\nType = 0x%X\nLength = %u\nData Pointer = %p\nOption count = %d\n---\n",
			pcapng->block_type, pcapng->block_total_lenght, (void*)pcapng->block_body, __option_count(pcapng->options));
	if (next != NULL)
		strcat(string, next);

	free(next);
	return string;
}

uint32_t *light_pcapng_to_memory(const light_pcapng pcapng, size_t *size)
{
	if (pcapng == NULL) {
		*size = 0;
		return NULL;
	}

	uint32_t *block_mem, *option_mem;
	size_t current_size = pcapng->block_total_lenght;
	size_t option_length, next_length;
	uint32_t *next_mem = light_pcapng_to_memory(pcapng->next_block, &next_length);
	size_t body_length = pcapng->block_total_lenght - 2 * sizeof(pcapng->block_total_lenght) - sizeof(pcapng->block_type);

	block_mem = calloc(current_size + next_length, 1);

	option_mem = __get_option_size(pcapng->options, &option_length);
	body_length -= option_length;

	block_mem[0] = pcapng->block_type;
	block_mem[1] = pcapng->block_total_lenght;
	memcpy(&block_mem[2], pcapng->block_body, body_length);
	memcpy(&block_mem[2 + body_length / 4], option_mem, option_length);
	block_mem[pcapng->block_total_lenght / 4 - 1] = pcapng->block_total_lenght;

	DCHECK_ASSERT(pcapng->block_total_lenght, body_length + option_length + 3 * sizeof(uint32_t), light_stop);
	memcpy(&block_mem[pcapng->block_total_lenght / 4], next_mem, next_length);

	free(next_mem);
	free(option_mem);

	*size = current_size + next_length;

	return block_mem;
}

int light_pcapng_validate(light_pcapng p0, uint32_t *p1)
{
	light_pcapng iterator0 = p0;
	uint32_t *iterator1 = p1;
	int block_count = 0;

	while (iterator0 != NULL && iterator1 != NULL) { // XXX find a better stop condition.
		if (iterator0->block_type != iterator1[0] ||
				iterator0->block_total_lenght != iterator1[1]) {
			fprintf(stderr, "Block type or length mismatch at block %d!\n", block_count);
			fprintf(stderr, "Expected type: 0x%X == 0x%X and expected length: %u == %u\n",
					iterator0->block_type, iterator1[0], iterator0->block_total_lenght, iterator1[1]);
			return 0;
		}
		size_t size = 0;
		light_pcapng next_block = iterator0->next_block;
		iterator0->next_block = NULL; // This might be quite intrusive.
		uint32_t *mem = light_pcapng_to_memory(iterator0, &size);
		if (memcmp(mem, iterator1, size) != 0) {
			iterator0->next_block = next_block;
			free(mem);
			fprintf(stderr, "Block contents mismatch!\n");
			return 0;
		}

		free(mem);
		iterator0->next_block = next_block;
		iterator0 = iterator0->next_block;

		iterator1 += iterator1[1] / sizeof(uint32_t);
		block_count++;
	}

	return 1;
}
