// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2022, Google Inc, Steven Rostedt <rostedt@goodmis.org>
*/
#include <stdlib.h>
#include <trace-cmd.h>
#include <trace-cmd-local.h>

struct filter {
	struct tep_event_filter		*filter;
};

struct tracecmd_filter {
	struct tep_handle	*tep;
	struct filter		**event_filters;
	struct filter		**event_notrace;
	bool			*last_printed;
	int			nr_cpus;
	int			nr_filters;
	int			nr_notrace;
	int			kernel_stacktrace_id;
	int			user_stacktrace_id;
};

static bool test_stacktrace(struct tracecmd_filter *filter, struct tep_record *record,
			    int stacktrace_id)
{
	struct tep_handle *tep = filter->tep;
	int id;

	if (stacktrace_id < 0)
		return false;

	id = tep_data_type(tep, record);
	if (id != stacktrace_id)
		return false;

	return filter->last_printed[record->cpu];
}

static bool test_stacktraces(struct tracecmd_filter *filter, struct tep_record *record)
{
	return test_stacktrace(filter, record, filter->kernel_stacktrace_id) ||
		test_stacktrace(filter, record, filter->user_stacktrace_id);
}

enum tracecmd_filters tracecmd_filter_match(struct tracecmd_filter *filter,
					    struct tep_record *record)
{
	bool found = false;
	int ret;
	int i;

	if (!filter)
		return TRACECMD_FILTER_NONE;

	/* Setup stack traces. If a event is shown, still show stack traces */
	if (!filter->kernel_stacktrace_id) {
		struct tep_handle *tep = filter->tep;
		struct tep_event *event;

		/* In case the below logic fails, do not do this again */
		filter->kernel_stacktrace_id = -1;

		event = tep_find_event_by_name(tep, "ftrace", "kernel_stack");
		if (event)
			filter->kernel_stacktrace_id = event->id;

		event = tep_find_event_by_name(tep, "ftrace", "user_stack");
		if (event)
			filter->user_stacktrace_id = event->id;

		filter->nr_cpus = tep_get_cpus(tep);
		filter->last_printed = calloc(filter->nr_cpus, sizeof(*filter->last_printed));
		if (!filter->last_printed) {
			tracecmd_warning("Could not allocate last_printed array for stack trace filtering");
			filter->kernel_stacktrace_id = -1;
			filter->user_stacktrace_id = -1;
		}
	}

	for (i = 0; i < filter->nr_filters; i++) {
		ret = tep_filter_match(filter->event_filters[i]->filter, record);
		switch (ret) {
		case TRACECMD_FILTER_NONE:
		case TRACECMD_FILTER_MATCH:
			found = true;
		}
		if (found)
			break;
	}

	if (!found && filter->nr_filters) {
		/* If this is a stack trace and the last event was printed continue */
		if (!test_stacktraces(filter, record))
			return TRACECMD_FILTER_MISS;
	}

	found = false;
	/* We need to test all negative filters */
	for (i = 0; i < filter->nr_notrace; i++) {
		ret = tep_filter_match(filter->event_notrace[i]->filter, record);
		switch (ret) {
		case TRACECMD_FILTER_NONE:
		case TRACECMD_FILTER_MATCH:
			found = true;
		}
		if (found)
			break;
	}

	if (filter->last_printed)
		filter->last_printed[record->cpu] = !found;

	return found ? TRACECMD_FILTER_MISS : TRACECMD_FILTER_MATCH;
}

struct tracecmd_filter *tracecmd_filter_add(struct tracecmd_input *handle,
					    const char *filter_str, bool neg)
{
	struct tracecmd_filter *trace_filter;
	struct tep_handle *tep;
	struct filter ***filter_ptr;
	struct filter **filters;
	struct filter *filter;
	int *nr;
	int ret;

	filter = calloc(1, sizeof(*filter));
	if (!filter)
		return NULL;

	tep = tracecmd_get_tep(handle);

	trace_filter = tracecmd_filter_get(handle);
	if (!trace_filter) {
		trace_filter = calloc(1, sizeof(*trace_filter));
		if (!trace_filter)
			goto fail;
		tracecmd_filter_set(handle, trace_filter);
		trace_filter->tep = tep;
	}

	filter->filter = tep_filter_alloc(tep);
	if (!filter->filter)
		goto fail;

	ret = tep_filter_add_filter_str(filter->filter, filter_str);
	if (ret < 0)
		goto fail;

	if (neg) {
		filter_ptr = &trace_filter->event_notrace;
		nr = &trace_filter->nr_notrace;
	} else {
		filter_ptr = &trace_filter->event_filters;
		nr = &trace_filter->nr_filters;
	}

	filters = realloc(*filter_ptr, sizeof(*filters) * (*nr + 1));
	if (!filters)
		goto fail;

	*filter_ptr = filters;
	filters[*nr] = filter;
	(*nr)++;
	return trace_filter;
 fail:
	if (filter) {
		tep_filter_free(filter->filter);
		free(filter);
	}
	return NULL;
}

static void free_filters (struct filter **filter, int nr)
{
	int i;

	for (i = 0; i < nr; i++) {
		tep_filter_free(filter[i]->filter);
		free(filter[i]);
	}

	free(filter);
}

__hidden void tracecmd_filter_free(struct tracecmd_filter *trace_filter)
{
	if (!trace_filter)
		return;

	free_filters(trace_filter->event_filters, trace_filter->nr_filters);
	free_filters(trace_filter->event_notrace, trace_filter->nr_notrace);

	free(trace_filter);
}
