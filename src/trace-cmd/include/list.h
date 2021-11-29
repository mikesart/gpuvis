/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright (C) 2009 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
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
