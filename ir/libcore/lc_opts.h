/*
 * This file is part of libFirm.
 * Copyright (C) 2012 IPD Goos, Universit"at Karlsruhe, Germany
 */

/*
   Option management library.
   This module can read (typed) options from a config file or
   parse a command line. The options are managed in a tree structure.
*/

#ifndef _LC_OPTS_H
#define _LC_OPTS_H

#include <stdio.h>
#include <stdbool.h>

#include "lc_printf.h"

/**
 * The type of an option.
 */
typedef enum {
	lc_opt_type_invalid,
	lc_opt_type_enum,
	lc_opt_type_bit,
	lc_opt_type_boolean,
	lc_opt_type_string,
	lc_opt_type_int,
	lc_opt_type_double
} lc_opt_type_t;

typedef struct lc_opt_entry_t lc_opt_entry_t;

typedef bool (lc_opt_callback_t)(const char *name, lc_opt_type_t type, void *data, size_t length, ...);

typedef int (lc_opt_dump_t)(char *buf, size_t n, const char *name, lc_opt_type_t type, void *data, size_t length);

typedef int (lc_opt_dump_vals_t)(char *buf, size_t n, const char *name, lc_opt_type_t type, void *data, size_t length);

typedef struct {
	const char *name;               /**< The name of the option. */
	const char *desc;               /**< A description for the option. */
	lc_opt_type_t type;             /**< The type of the option (see enum). */
	void *value;                    /**< A pointer to the area, where the value
	                                     of the option shall be put to. May be
	                                     NULL. */

	size_t len;                     /**< The amount of bytes available at the
	                                     location value points to. */
	lc_opt_callback_t *cb;          /**< A callback that is called, when the
	                                     option is set. This may never be NULL. */

	lc_opt_dump_t *dump;            /**< A function which is able to format the
	                                     options value into a string. May be
	                                     NULL. */

	lc_opt_dump_vals_t *dump_vals;  /**< A function which is able to format the possible values
	                                     for this option into a string. May be NULL. */


} lc_opt_table_entry_t;

#define _LC_OPT_ENT(name, desc, type, val_type, value, len, cb, dump, dump_vals) \
	{ name, desc, type, 1 ? (value) : (val_type*)0 /* Produces a warning, if var has wrong type. */, len, cb, dump, dump_vals }

#define LC_OPT_ENT_INT(name, desc, addr) \
	_LC_OPT_ENT(name, desc, lc_opt_type_int, int, addr, 0, lc_opt_std_cb, lc_opt_std_dump, NULL)

#define LC_OPT_ENT_DBL(name, desc, addr) \
	_LC_OPT_ENT(name, desc, lc_opt_type_double, double, addr, 0, lc_opt_std_cb, lc_opt_std_dump, NULL)

#define LC_OPT_ENT_BIT(name, desc, addr, mask) \
	_LC_OPT_ENT(name, desc, lc_opt_type_bit, unsigned, addr, mask, lc_opt_std_cb, lc_opt_std_dump, NULL)

#define LC_OPT_ENT_BOOL(name, desc, addr) \
	_LC_OPT_ENT(name, desc, lc_opt_type_boolean, int, addr, 0, lc_opt_std_cb, lc_opt_std_dump, lc_opt_bool_dump_vals)

typedef char lc_opt_str_t[];
#define LC_OPT_ENT_STR(name, desc, buf) \
	_LC_OPT_ENT(name, desc, lc_opt_type_string, lc_opt_str_t, buf, sizeof(*buf), lc_opt_std_cb, lc_opt_std_dump, NULL)

#define LC_OPT_LAST \
	_LC_OPT_ENT(NULL, NULL, lc_opt_type_invalid, void, NULL, 0, NULL, NULL, NULL)

/**
 * Get the root option group.
 * @return The root option group.
 */
lc_opt_entry_t *lc_opt_root_grp(void);

/**
 * Check, if a group is the root group
 * @param ent   The entry to check for.
 * @return      1, if the entry is the root group, 0 otherwise.
 */
int lc_opt_grp_is_root(const lc_opt_entry_t *ent);

/**
 * Get an option group.
 * If the group is not already present, it is created.
 * @param parent   The parent group to look in.
 * @param name     The name of the group to lookup
 * @return         The already present or created group.
 */
lc_opt_entry_t *lc_opt_get_grp(lc_opt_entry_t *parent, const char *name);

/**
 * Add an option to a group.
 * @param grp       The group to add the option to.
 * @param name      The name of the option (must be unique inside the group).
 * @param desc      A description of the option.
 * @param type      The data type of the option (see lc_opt_type_*)
 * @param value     A pointer to the memory, where the value shall be stored.
 *                  (May be NULL).
 * @param length    Amount of bytes available at the memory location
 *                  indicated by @p value.
 * @param cb        A callback function to be called, as the option's value
 *                  is set (may be NULL).
 * @param err       Error information to be set (may be NULL).
 * @return          The handle for the option.
 */
lc_opt_entry_t *lc_opt_add_opt(lc_opt_entry_t *grp,
                               const char *name,
                               const char *desc,
                               lc_opt_type_t type,
                               void *value, size_t length,
                               lc_opt_callback_t *cb,
                               lc_opt_dump_t *dump,
                               lc_opt_dump_vals_t *dump_vals);

bool lc_opt_std_cb(const char *name, lc_opt_type_t type, void *data, size_t length, ...);

int lc_opt_std_dump(char *buf, size_t n, const char *name, lc_opt_type_t type, void *data, size_t length);

int lc_opt_bool_dump_vals(char *buf, size_t n, const char *name, lc_opt_type_t type, void *data, size_t length);

#define lc_opt_add_opt_int(grp, name, desc, value, err) \
	lc_opt_add_opt(grp, name, desc, lc_opt_type_int, value, 0, lc_opt_std_cb, lc_opt_std_dump, NULL, err)

#define lc_opt_add_opt_double(grp, name, desc, value, err) \
	lc_opt_add_opt(grp, name, desc, lc_opt_type_double, value, 0, lc_opt_std_cb, lc_opt_std_dump, NULL, err)

#define lc_opt_add_opt_string(grp, name, desc, buf, len, err) \
	lc_opt_add_opt(grp, name, desc, lc_opt_type_string, buf, len, lc_opt_std_cb, lc_opt_std_dump, NULL, err)

#define lc_opt_add_opt_bit(grp, name, desc, value, mask, err) \
	lc_opt_add_opt(grp, name, desc, lc_opt_type_bit, value, mask, lc_opt_std_cb, lc_opt_std_dump, NULL, err)


/**
 * Find a group inside another group.
 * @param grp   The group to search inside.
 * @param name  The name of the group you are looking for.
 * @return      The group or NULL, if no such group can be found.
 */
lc_opt_entry_t *lc_opt_find_grp(const lc_opt_entry_t *grp, const char *name);

/**
 * Find an option inside another group.
 * @param grp   The group to search inside.
 * @param name  The name of the option you are looking for.
 * @return      The group or NULL, if no such option can be found.
 */
lc_opt_entry_t *lc_opt_find_opt(const lc_opt_entry_t *grp, const char *name);

/**
 * Resolve a group.
 * @param root   The group to start resolving from.
 * @param names  A string array containing the path to the group.
 * @param n      Number of entries in @p names to consider.
 * @return       The group or NULL, if none is found.
 */
lc_opt_entry_t *lc_opt_resolve_grp(const lc_opt_entry_t *root,
                                   const char * const *names, int n);

/**
 * Resolve an option.
 * @param root   The group to start resolving from.
 * @param names  A string array containing the path to the option.
 * @param n      Number of entries in @p names to consider.
 * @return       The option or NULL, if none is found.
 */
lc_opt_entry_t *lc_opt_resolve_opt(const lc_opt_entry_t *root,
                                   const char * const *names, int n);

/**
 * Set the value of an option.
 * @param opt    The option to set.
 * @param value  The value of the option in a string representation.
 * @return       0, if an error occurred, 1 else.
 */
bool lc_opt_occurs(lc_opt_entry_t *opt, const char *value);

/**
 * Convert the option to a string representation.
 * @param buf  The string buffer to put the string representation to.
 * @param len  The length of @p buf.
 * @param ent  The option to process.
 * @return     @p buf.
 */
char *lc_opt_value_to_string(char *buf, size_t len, const lc_opt_entry_t *ent);

/**
 * Get the name of the type of an option.
 * @param ent The option.
 * @return The name of the type of the option.
 */
const char *lc_opt_get_type_name(const lc_opt_entry_t *ent);

/**
 * Print the help screen for the given entity to the given file.
 */
void lc_opt_print_help(lc_opt_entry_t *ent, FILE *f);

/**
 * Print the help screen for the given entity to the given file.
 * Use separator instead of '.' and ignore entities above ent,
 * i.e. if ent is root.be and has option isa.mach, prints
 * isa<separator>mach instead of root.be.isa.mach
 */
void lc_opt_print_help_for_entry(lc_opt_entry_t *ent, char separator, FILE *f);

void lc_opt_print_tree(lc_opt_entry_t *ent, FILE *f);

bool lc_opt_add_table(lc_opt_entry_t *grp, const lc_opt_table_entry_t *table);

/**
 * Set options from a single (command line) argument.
 * @param root          The root group we start resolving from.
 * @param arg           The command line argument itself.
 * @return              1, if the argument was set, 0 if not.
 */
int lc_opt_from_single_arg(const lc_opt_entry_t *grp, const char *arg);

#endif
