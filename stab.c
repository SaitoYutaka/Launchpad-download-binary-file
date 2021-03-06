/* MSPDebug - debugging tool for the eZ430
 * Copyright (C) 2009, 2010 Daniel Beer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <regex.h>
#include <errno.h>
#include <assert.h>
#include "btree.h"
#include "stab.h"
#include "util.h"
#include "output.h"

stab_t stab_default;

/************************************************************************
 * B+Tree definitions
 */

struct sym_key {
	char name[64];
};

static const struct sym_key sym_key_zero = {
	.name = {0}
};

static int sym_key_compare(const void *left, const void *right)
{
	return strcmp(((const struct sym_key *)left)->name,
		      ((const struct sym_key *)right)->name);
}

static void sym_key_init(struct sym_key *key, const char *text)
{
	int len = strlen(text);

	if (len >= sizeof(key->name))
		len = sizeof(key->name) - 1;

	memcpy(key->name, text, len);
	key->name[len] = 0;
}

struct addr_key {
	address_t      addr;
	char           name[64];
};

static const struct addr_key addr_key_zero = {
	.addr = 0,
	.name = {0}
};

static int addr_key_compare(const void *left, const void *right)
{
	const struct addr_key *kl = (const struct addr_key *)left;
	const struct addr_key *kr = (const struct addr_key *)right;

	if (kl->addr < kr->addr)
		return -1;
	if (kl->addr > kr->addr)
		return 1;

	return strcmp(kl->name, kr->name);
}

static void addr_key_init(struct addr_key *key, address_t addr,
			  const char *text)
{
	int len = strlen(text);

	if (len >= sizeof(key->name))
		len = sizeof(key->name) - 1;

	key->addr = addr;
	memcpy(key->name, text, len);
	key->name[len] = 0;
}

static const struct btree_def sym_table_def = {
	.compare = sym_key_compare,
	.zero = &sym_key_zero,
	.branches = 32,
	.key_size = sizeof(struct sym_key),
	.data_size = sizeof(address_t)
};

static const struct btree_def addr_table_def = {
	.compare = addr_key_compare,
	.zero = &addr_key_zero,
	.branches = 32,
	.key_size = sizeof(struct addr_key),
	.data_size = 0
};

/************************************************************************
 * Symbol table methods
 */

struct stab {
	btree_t         sym;
	btree_t         addr;
};

void stab_clear(stab_t st)
{
	btree_clear(st->sym);
	btree_clear(st->addr);
}

int stab_set(stab_t st, const char *name, int value)
{
	struct sym_key skey;
	struct addr_key akey;
	address_t addr = value;
	address_t old_addr;

	sym_key_init(&skey, name);

	/* Look for an old address first, and delete the reverse mapping
	 * if it's there.
	 */
	if (!btree_get(st->sym, &skey, &old_addr)) {
		addr_key_init(&akey, old_addr, skey.name);
		btree_delete(st->addr, &akey);
	}

	/* Put the new mapping into both tables */
	addr_key_init(&akey, addr, name);
	if (btree_put(st->addr, &akey, NULL) < 0 ||
	    btree_put(st->sym, &skey, &addr) < 0) {
		printc_err("stab: can't set %s = 0x%04x\n", name, addr);
		return -1;
	}

	return 0;
}

int stab_nearest(stab_t st, address_t addr, char *ret_name, int max_len,
		 address_t *ret_offset)
{
	struct addr_key akey;
	int i;

	akey.addr = addr;
	for (i = 0; i < sizeof(akey.name); i++)
		akey.name[i] = 0xff;
	akey.name[sizeof(akey.name) - 1] = 0xff;

	if (!btree_select(st->addr, &akey, BTREE_LE, &akey, NULL)) {
		strncpy(ret_name, akey.name, max_len);
		ret_name[max_len - 1] = 0;
		*ret_offset = addr - akey.addr;
		return 0;
	}

	return -1;
}

int stab_get(stab_t st, const char *name, address_t *value)
{
	struct sym_key skey;
	address_t addr;

	sym_key_init(&skey, name);
	if (btree_get(st->sym, &skey, &addr))
		return -1;

	*value = addr;
	return 0;
}

int stab_del(stab_t st, const char *name)
{
	struct sym_key skey;
	address_t value;
	struct addr_key akey;

	sym_key_init(&skey, name);
	if (btree_get(st->sym, &skey, &value))
		return -1;

	addr_key_init(&akey, value, name);
	btree_delete(st->sym, &skey);
	btree_delete(st->addr, &akey);

	return 0;
}

int stab_enum(stab_t st, stab_callback_t cb, void *user_data)
{
	int ret;
	struct addr_key akey;

	ret = btree_select(st->addr, NULL, BTREE_FIRST,
			   &akey, NULL);
	while (!ret) {
		if (cb(user_data, akey.name, akey.addr) < 0)
			return -1;
		ret = btree_select(st->addr, NULL, BTREE_NEXT,
				   &akey, NULL);
	}

	return 0;
}

stab_t stab_new(void)
{
	stab_t st = malloc(sizeof(*st));

	if (!st) {
		pr_error("stab: failed to allocate memory\n");
		return NULL;
	}

	st->sym = btree_alloc(&sym_table_def);
	if (!st->sym) {
		printc_err("stab: failed to allocate symbol table\n");
		free(st);
		return NULL;
	}

	st->addr = btree_alloc(&addr_table_def);
	if (!st->addr) {
		printc_err("stab: failed to allocate address table\n");
		btree_free(st->sym);
		free(st);
		return NULL;
	}

        return st;
}

void stab_destroy(stab_t st)
{
	btree_free(st->sym);
	btree_free(st->addr);
	free(st);
}
