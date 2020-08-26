/*
 * libifupdown/lifecycle.c
 * Purpose: management of interface lifecycle (bring up, takedown, reload)
 *
 * Copyright (c) 2020 Ariadne Conill <ariadne@dereferenced.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * This software is provided 'as is' and without any warranty, express or
 * implied.  In no event shall the authors be liable for any damages arising
 * from the use of this software.
 */

#include <ctype.h>
#include <string.h>

#include "libifupdown/environment.h"
#include "libifupdown/execute.h"
#include "libifupdown/interface.h"
#include "libifupdown/lifecycle.h"
#include "libifupdown/state.h"
#include "libifupdown/tokenize.h"

static bool
handle_commands_for_phase(const struct lif_execute_opts *opts, char *const envp[], struct lif_interface *iface, const char *phase)
{
	struct lif_node *iter;

	LIF_DICT_FOREACH(iter, &iface->vars)
	{
		struct lif_dict_entry *entry = iter->data;

		if (strcmp(entry->key, phase))
			continue;

		const char *cmd = entry->data;
		if (!lif_execute_fmt(opts, envp, "%s", cmd))
			return false;
	}

	return true;
}

static inline bool
handle_single_executor_for_phase(const struct lif_dict_entry *entry, const struct lif_execute_opts *opts, char *const envp[], const char *phase)
{
	if (strcmp(entry->key, "use"))
		return true;

	const char *cmd = entry->data;
	if (!lif_maybe_run_executor(opts, envp, cmd, phase))
		return false;

	return true;
}

static bool
handle_executors_for_phase(const struct lif_execute_opts *opts, char *const envp[], struct lif_interface *iface, bool up, const char *phase)
{
	struct lif_node *iter;

	if (up)
	{
		LIF_DICT_FOREACH(iter, &iface->vars)
			handle_single_executor_for_phase(iter->data, opts, envp, phase);
	}
	else
	{
		LIF_DICT_FOREACH_REVERSE(iter, &iface->vars)
			handle_single_executor_for_phase(iter->data, opts, envp, phase);
	}

	return true;
}

static bool
query_dependents_from_executors(const struct lif_execute_opts *opts, char *const envp[], struct lif_interface *iface, char *buf, size_t bufsize, const char *phase)
{
	struct lif_node *iter;

	LIF_DICT_FOREACH(iter, &iface->vars)
	{
		char resbuf[1024] = {};
		struct lif_dict_entry *entry = iter->data;
		struct lif_execute_opts exec_opts = {
			.verbose = opts->verbose,
			.executor_path = opts->executor_path,
			.interfaces_file = opts->interfaces_file
		};

		if (strcmp(entry->key, "use"))
			continue;

		const char *cmd = entry->data;
		if (!lif_maybe_run_executor_with_result(&exec_opts, envp, cmd, resbuf, sizeof resbuf, phase))
			return false;

		if (!*resbuf)
			continue;

		strlcat(buf, " ", bufsize);
		strlcat(buf, resbuf, bufsize);
	}

	return true;
}

static void
build_environment(char **envp[], const struct lif_execute_opts *opts, struct lif_interface *iface, const char *lifname, const char *phase, const char *mode)
{
	if (lifname == NULL)
		lifname = iface->ifname;

	lif_environment_push(envp, "IFACE", lifname);
	lif_environment_push(envp, "PHASE", phase);
	lif_environment_push(envp, "MODE", mode);
	lif_environment_push(envp, "METHOD", "none");

	if (opts->verbose)
		lif_environment_push(envp, "VERBOSE", "1");

	if (opts->interfaces_file)
		lif_environment_push(envp, "INTERFACES_FILE", opts->interfaces_file);

	struct lif_node *iter;
	bool did_address = false, did_gateway = false;

	LIF_DICT_FOREACH(iter, &iface->vars)
	{
		struct lif_dict_entry *entry = iter->data;

		if (!strcmp(entry->key, "address"))
		{
			if (did_address)
				continue;

			struct lif_address *addr = entry->data;
			char addrbuf[4096];

			if (!lif_address_unparse(addr, addrbuf, sizeof addrbuf, true))
				continue;

			lif_environment_push(envp, "IF_ADDRESS", addrbuf);
			did_address = true;

			continue;
		}
		else if (!strcmp(entry->key, "gateway"))
		{
			if (did_gateway)
				continue;

			did_gateway = true;
		}
		else if (!strcmp(entry->key, "requires"))
		{
			if (iface->is_bridge)
				lif_environment_push(envp, "IF_BRIDGE_PORTS", (const char *) entry->data);

			if (iface->is_bond)
				lif_environment_push(envp, "IF_BOND_SLAVES", (const char *) entry->data);
		}

		char envkey[4096] = "IF_";
		strlcat(envkey, entry->key, sizeof envkey);
		char *ep = envkey + 2;

		while (*ep++)
		{
			*ep = toupper(*ep);

			if (*ep == '-')
				*ep = '_';
		}

		lif_environment_push(envp, envkey, (const char *) entry->data);
	}
}

bool
lif_lifecycle_query_dependents(const struct lif_execute_opts *opts, struct lif_interface *iface, const char *lifname)
{
	char deps[4096] = {};
	char final_deps[4096] = {};

	if (lifname == NULL)
		lifname = iface->ifname;

	char **envp = NULL;

	build_environment(&envp, opts, iface, lifname, "depend", "depend");

	struct lif_dict_entry *entry = lif_dict_find(&iface->vars, "requires");
	if (entry != NULL)
		strlcpy(deps, entry->data, sizeof deps);

	if (!query_dependents_from_executors(opts, envp, iface, deps, sizeof deps, "depend"))
		return false;

	char *p = deps;
	while (*p)
	{
		char *token = lif_next_token(&p);

		if (strstr(final_deps, token) != NULL)
			continue;

		strlcat(final_deps, token, sizeof final_deps);
		strlcat(final_deps, " ", sizeof final_deps);
	}

	if (entry != NULL)
	{
		free(entry->data);
		entry->data = strdup(final_deps);
	}
	else if (*final_deps)
		lif_dict_add(&iface->vars, "requires", strdup(final_deps));

	lif_environment_free(&envp);

	return true;
}

bool
lif_lifecycle_run_phase(const struct lif_execute_opts *opts, struct lif_interface *iface, const char *phase, const char *lifname, bool up)
{
	char **envp = NULL;

	build_environment(&envp, opts, iface, lifname, phase, up ? "start" : "stop");

	if (!handle_executors_for_phase(opts, envp, iface, up, phase))
		goto handle_error;

	if (!handle_commands_for_phase(opts, envp, iface, phase))
		goto handle_error;

	/* we should do error handling here, but ifupdown1 doesn't */
	lif_execute_fmt(opts, envp, "/bin/run-parts /etc/network/if-%s.d", phase);

	lif_environment_free(&envp);
	return true;

handle_error:
	lif_environment_free(&envp);
	return false;
}

static bool
handle_dependents(const struct lif_execute_opts *opts, struct lif_interface *parent, struct lif_dict *collection, struct lif_dict *state, bool up)
{
	struct lif_dict_entry *requires = lif_dict_find(&parent->vars, "requires");

	/* no dependents, nothing to worry about */
	if (requires == NULL)
		return true;

	char require_ifs[4096] = {};
	strlcpy(require_ifs, requires->data, sizeof require_ifs);
	char *bufp = require_ifs;

	for (char *tokenp = lif_next_token(&bufp); *tokenp; tokenp = lif_next_token(&bufp))
	{
		struct lif_interface *iface = lif_interface_collection_find(collection, tokenp);

		/* already up or down, skip */
		if (up == iface->is_up)
		{
			if (opts->verbose)
				fprintf(stderr, "ifupdown: skipping dependent interface %s (of %s)\n",
					iface->ifname, parent->ifname);
			continue;
		}

		if (opts->verbose)
			fprintf(stderr, "ifupdown: changing state of dependent interface %s (of %s) to %s\n",
				iface->ifname, parent->ifname, up ? "up" : "down");

		if (!lif_lifecycle_run(opts, iface, collection, state, iface->ifname, up))
			return false;
	}

	return true;
}

bool
lif_lifecycle_run(const struct lif_execute_opts *opts, struct lif_interface *iface, struct lif_dict *collection, struct lif_dict *state, const char *lifname, bool up)
{
	if (lifname == NULL)
		lifname = iface->ifname;

	if (!lif_lifecycle_query_dependents(opts, iface, lifname))
		return false;

	if (up)
	{
		/* when going up, dependents go up first. */
		if (!handle_dependents(opts, iface, collection, state, up))
			return false;

		/* XXX: we should try to recover (take the iface down) if bringing it up fails.
		 * but, right now neither debian ifupdown or busybox ifupdown do any recovery,
		 * so we wont right now.
		 */
		if (!lif_lifecycle_run_phase(opts, iface, "pre-up", lifname, up))
			return false;

		if (!lif_lifecycle_run_phase(opts, iface, "up", lifname, up))
			return false;

		if (!lif_lifecycle_run_phase(opts, iface, "post-up", lifname, up))
			return false;

		lif_state_upsert(state, lifname, iface);

		iface->is_up = true;
	}
	else
	{
		if (!lif_lifecycle_run_phase(opts, iface, "pre-down", lifname, up))
			return false;

		if (!lif_lifecycle_run_phase(opts, iface, "down", lifname, up))
			return false;

		if (!lif_lifecycle_run_phase(opts, iface, "post-down", lifname, up))
			return false;

		/* when going up, dependents go down last. */
		if (!handle_dependents(opts, iface, collection, state, up))
			return false;

		lif_state_delete(state, lifname);

		iface->is_up = false;
	}

	return true;
}
