/*
 * Copyright (C) 2009 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not,  see <http://www.gnu.org/licenses>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#ifndef __LIST_H
#define __LIST_H

#define offset_of(type, field)		(long)(&((type *)0)->field)
#define container_of(p, type, field)	(type *)((long)p - offset_of(type, field))

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

static inline void list_head_init(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void list_add(struct list_head *p, struct list_head *head)
{
	struct list_head *next = head->next;

	p->prev = head;
	p->next = next;
	next->prev = p;
	head->next = p;
}

static inline void list_add_tail(struct list_head *p, struct list_head *head)
{
	struct list_head *prev = head->prev;

	p->prev = prev;
	p->next = head;
	prev->next = p;
	head->prev = p;
}

static inline void list_del(struct list_head *p)
{
	struct list_head *next = p->next;
	struct list_head *prev = p->prev;

	next->prev = prev;
	prev->next = next;
}

static inline int list_empty(struct list_head *list)
{
	return list->next == list;
}

#define list_for_each_entry(p, list, field)		\
	for (p = container_of((list)->next, typeof(*p), field);	\
	     &(p)->field != list;				\
	     p = container_of((p)->field.next, typeof(*p), field))

#define list_for_each_entry_safe(p, n, list, field)			\
	for (p = container_of((list)->next, typeof(*p), field),		\
		     n = container_of((p)->field.next, typeof(*p), field); \
	     &(p)->field != list;					\
	     p = n, n = container_of((p)->field.next, typeof(*p), field))

#endif /* __LIST_H */
