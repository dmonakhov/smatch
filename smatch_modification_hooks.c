/*
 * Copyright (C) 2009 Dan Carpenter.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see http://www.gnu.org/copyleft/gpl.txt
 */

/*
 * There are a number of ways that variables are modified:
 * 1) assignment
 * 2) increment/decrement
 * 3) assembly
 * 4) inside functions.
 *
 * For setting stuff inside a function then, of course, it's more accurate if
 * you have the cross function database built.  Otherwise we are super
 * aggressive about marking things as modified and if you have "frob(foo);" then
 * we assume "foo->bar" is modified.
 */

#include <stdlib.h>
#include <stdio.h>
#include "smatch.h"
#include "smatch_extra.h"
#include "smatch_slist.h"

enum {
	match_none = 0,
	match_exact,
	match_indirect
};

enum {
	EARLY = 0,
	LATE = 1,
	BOTH = 2
};

static modification_hook **hooks;
static modification_hook **indirect_hooks;  /* parent struct modified etc */
static modification_hook **hooks_late;
static modification_hook **indirect_hooks_late;  /* parent struct modified etc */

ALLOCATOR(modification_data, "modification data");

static int my_id;
static struct smatch_state *alloc_my_state(struct expression *expr, struct smatch_state *prev)
{
	struct smatch_state *state;
	struct modification_data *data;
	char *name;

	state = __alloc_smatch_state(0);
	expr = strip_expr(expr);
	name = expr_to_str(expr);
	state->name = alloc_sname(name);
	free_string(name);

	data = __alloc_modification_data(0);
	data->prev = prev;
	data->cur = expr;
	state->data = data;

	return state;
}

void add_modification_hook(int owner, modification_hook *call_back)
{
	hooks[owner] = call_back;
}

void add_indirect_modification_hook(int owner, modification_hook *call_back)
{
	indirect_hooks[owner] = call_back;
}

void add_modification_hook_late(int owner, modification_hook *call_back)
{
	hooks_late[owner] = call_back;
}

void add_indirect_modification_hook_late(int owner, modification_hook *call_back)
{
	indirect_hooks_late[owner] = call_back;
}

static int matches(char *name, struct symbol *sym, struct sm_state *sm)
{
	int len;

	if (sym != sm->sym)
		return match_none;

	len = strlen(name);
	if (strncmp(sm->name, name, len) == 0) {
		if (sm->name[len] == '\0')
			return match_exact;
		if (sm->name[len] == '-' || sm->name[len] == '.')
			return match_indirect;
	}
	if (sm->name[0] != '*')
		return match_none;
	if (strncmp(sm->name + 1, name, len) == 0) {
		if (sm->name[len + 1] == '\0')
			return match_indirect;
		if (sm->name[len + 1] == '-' || sm->name[len + 1] == '.')
			return match_indirect;
	}
	return match_none;
}

static void call_modification_hooks_name_sym(char *name, struct symbol *sym, struct expression *mod_expr, int late)
{
	struct sm_state *sm;
	struct smatch_state *prev;
	int match;

	prev = get_state(my_id, name, sym);
	set_state(my_id, name, sym, alloc_my_state(mod_expr, prev));

	FOR_EACH_SM(__get_cur_stree(), sm) {
		if (sm->owner > num_checks)
			continue;
		match = matches(name, sym, sm);
		if (!match)
			continue;

		if (late == EARLY || late == BOTH) {
			if (hooks[sm->owner])
				(hooks[sm->owner])(sm, mod_expr);
			if (match == match_indirect && indirect_hooks[sm->owner])
				(indirect_hooks[sm->owner])(sm, mod_expr);
		}
		if (late == LATE || late == BOTH) {
			if (hooks_late[sm->owner])
				(hooks_late[sm->owner])(sm, mod_expr);
			if (match == match_indirect && indirect_hooks_late[sm->owner])
				(indirect_hooks_late[sm->owner])(sm, mod_expr);
		}

	} END_FOR_EACH_SM(sm);
}

static void call_modification_hooks(struct expression *expr, struct expression *mod_expr, int late)
{
	char *name;
	struct symbol *sym;

	name = expr_to_known_chunk_sym(expr, &sym);
	if (!name)
		goto free;
	call_modification_hooks_name_sym(name, sym, mod_expr, late);
free:
	free_string(name);
}

static void db_param_add(struct expression *expr, int param, char *key, char *value)
{
	struct expression *arg;
	char *name;
	struct symbol *sym;

	while (expr->type == EXPR_ASSIGNMENT)
		expr = strip_expr(expr->right);
	if (expr->type != EXPR_CALL)
		return;

	arg = get_argument_from_call_expr(expr->args, param);
	if (!arg)
		return;

	name = get_variable_from_key(arg, key, &sym);
	if (!name || !sym)
		goto free;

	call_modification_hooks_name_sym(name, sym, expr, BOTH);
free:
	free_string(name);
}

static void match_assign(struct expression *expr, int late)
{
	call_modification_hooks(expr->left, expr, late);
}

static void unop_expr(struct expression *expr, int late)
{
	if (expr->op != SPECIAL_DECREMENT && expr->op != SPECIAL_INCREMENT)
		return;

	call_modification_hooks(expr->unop, expr, late);
}

static void match_call(struct expression *expr)
{
	struct expression *arg, *tmp;

	FOR_EACH_PTR(expr->args, arg) {
		tmp = strip_expr(arg);
		if (tmp->type == EXPR_PREOP && tmp->op == '&')
			call_modification_hooks(tmp->unop, expr, BOTH);
		else if (option_no_db)
			call_modification_hooks(deref_expression(tmp), expr, BOTH);
	} END_FOR_EACH_PTR(arg);
}

static void asm_expr(struct statement *stmt, int late)
{
	struct expression *expr;
	int state = 0;

	FOR_EACH_PTR(stmt->asm_outputs, expr) {
		switch (state) {
		case 0: /* identifier */
		case 1: /* constraint */
			state++;
			continue;
		case 2: /* expression */
			state = 0;
			call_modification_hooks(expr, NULL, late);
			continue;
		}
	} END_FOR_EACH_PTR(expr);
}


static void match_assign_early(struct expression *expr)
{
	match_assign(expr, EARLY);
}

static void unop_expr_early(struct expression *expr)
{
	unop_expr(expr, EARLY);
}

static void asm_expr_early(struct statement *stmt)
{
	asm_expr(stmt, EARLY);
}

static void match_assign_late(struct expression *expr)
{
	match_assign(expr, LATE);
}

static void unop_expr_late(struct expression *expr)
{
	unop_expr(expr, LATE);
}

static void asm_expr_late(struct statement *stmt)
{
	asm_expr(stmt, LATE);
}

struct smatch_state *get_modification_state(struct expression *expr)
{
	return get_state_expr(my_id, expr);
}

void register_modification_hooks(int id)
{
	my_id = id;

	hooks = malloc((num_checks + 1) * sizeof(*hooks));
	memset(hooks, 0, (num_checks + 1) * sizeof(*hooks));
	indirect_hooks = malloc((num_checks + 1) * sizeof(*hooks));
	memset(indirect_hooks, 0, (num_checks + 1) * sizeof(*hooks));
	hooks_late = malloc((num_checks + 1) * sizeof(*hooks));
	memset(hooks_late, 0, (num_checks + 1) * sizeof(*hooks));
	indirect_hooks_late = malloc((num_checks + 1) * sizeof(*hooks));
	memset(indirect_hooks_late, 0, (num_checks + 1) * sizeof(*hooks));

	add_hook(&match_assign_early, ASSIGNMENT_HOOK);
	add_hook(&unop_expr_early, OP_HOOK);
	add_hook(&asm_expr_early, ASM_HOOK);
}

void register_modification_hooks_late(int id)
{
	add_hook(&match_call, FUNCTION_CALL_HOOK);

	add_hook(&match_assign_late, ASSIGNMENT_HOOK_AFTER);
	add_hook(&unop_expr_late, OP_HOOK);
	add_hook(&asm_expr_late, ASM_HOOK);

	select_return_states_hook(PARAM_ADD, &db_param_add);
	select_return_states_hook(PARAM_SET, &db_param_add);
}

