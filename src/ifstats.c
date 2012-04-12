/***

ifstats.c	- the interface statistics module
Written by Gerard Paul Java
Copyright (c) Gerard Paul Java 1997-2002

This software is open source; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License in the included COPYING file for
details.
***/

#include "iptraf-ng-compat.h"
#include "tui/tui.h"

#include "ifstats.h"
#include "ifaces.h"
#include "isdntab.h"
#include "fltdefs.h"
#include "fltselect.h"
#include "packet.h"
#include "options.h"
#include "log.h"
#include "dirs.h"
#include "deskman.h"
#include "attrs.h"
#include "serv.h"
#include "timer.h"
#include "instances.h"
#include "logvars.h"
#include "promisc.h"
#include "error.h"

#define SCROLLUP 0
#define SCROLLDOWN 1

extern int exitloop;
extern int daemonized;

/* from log.c, applicable only to this module */

void writegstatlog(struct iftab *table, int unit, unsigned long nsecs,
		   FILE * logfile);

/*
 * USR1 log-rotation signal handlers
 */

void rotate_gstat_log(int s __unused)
{
	rotate_flag = 1;
	strcpy(target_logname, GSTATLOG);
	signal(SIGUSR1, rotate_gstat_log);
}

/*
 * Function to check if an interface is already in the interface list.
 * This eliminates duplicate interface entries due to aliases
 */

int ifinlist(struct iflist *list, char *ifname)
{
	struct iflist *ptmp = list;
	int result = 0;

	while ((ptmp != NULL) && (result == 0)) {
		result = (strcmp(ifname, ptmp->ifname) == 0);
		ptmp = ptmp->next_entry;
	}

	return result;
}

/*
 * Initialize the list of interfaces.  This linked list is used in the
 * selection boxes as well as in the general interface statistics screen.
 *
 * This function parses the /proc/net/dev file and grabs the interface names
 * from there.  The SIOGIFFLAGS ioctl() call is used to determine whether the
 * interfaces are active.  Inactive interfaces are omitted from selection
 * lists.
 */

void initiflist(struct iflist **list)
{
	FILE *fd;
	char ifname[IFNAMSIZ];
	struct iflist *itmp = NULL;
	struct iflist *tail = NULL;
	unsigned int index = 0;

	*list = NULL;

	fd = open_procnetdev();
	if (fd == NULL) {
		tui_error(ANYKEY_MSG, "Unable to obtain interface list");
		return;
	}

	while (get_next_iface(fd, ifname, sizeof(ifname))) {
		if (!*ifname)
			continue;

		if (ifinlist(*list, ifname))	/* ignore entry if already in */
			continue;	/* interface list */

		/*
		 * Check if the interface is actually up running.  This prevents
		 * inactive devices in /proc/net/dev from actually appearing in
		 * interface lists used by IPTraf.
		 */

		if (!iface_up(ifname))
			continue;

		int ifindex = iface_get_ifindex(ifname);
		if (ifindex < 0)
			continue;
		/*
		 * At this point, the interface is now sure to be up and running.
		 */

		itmp = xmalloc(sizeof(struct iflist));
		memset(itmp, 0, sizeof(struct iflist));
		strcpy(itmp->ifname, ifname);
		itmp->ifindex = ifindex;
		index++;
		itmp->index = index;

		if (*list == NULL) {
			*list = itmp;
			itmp->prev_entry = NULL;
		} else {
			tail->next_entry = itmp;
			itmp->prev_entry = tail;
		}

		tail = itmp;
		itmp->next_entry = NULL;
	}

	fclose(fd);
}

struct iflist *positionptr(struct iflist *iflist, const int ifindex)
{
	struct iflist *ptmp = iflist;
	struct iflist *last = ptmp;

	while ((ptmp != NULL) && (ptmp->ifindex != ifindex)) {
		last = ptmp;
		ptmp = ptmp->next_entry;
	}
	/* no interface was found, try to create new one */
	if (ptmp == NULL) {
		struct iflist *itmp = xmallocz(sizeof(struct iflist));
		itmp->ifindex = ifindex;
		itmp->index = last->index + 1;
		int r = iface_get_ifname(ifindex, itmp->ifname);
		if (r != 0) {
			write_error("Error getting interface name");
			return(NULL);
		}

		/* last can't be NULL otherwise we will have empty iflist */
		last->next_entry = itmp;
		itmp->prev_entry = last;
		itmp->next_entry = NULL;
		ptmp = itmp;
	}
	return(ptmp);
}

void destroyiflist(struct iflist *list)
{
	struct iflist *ctmp;
	struct iflist *ptmp;

	if (list != NULL) {
		ptmp = list;
		ctmp = ptmp->next_entry;

		do {
			free(ptmp);
			ptmp = ctmp;
			if (ctmp != NULL)
				ctmp = ctmp->next_entry;
		} while (ptmp != NULL);
	}
}

void no_ifaces_error(void)
{
	write_error("No active interfaces. Check their status or the /proc filesystem");
}

void updaterates(struct iftab *table, int unit, time_t starttime, time_t now,
		 unsigned int idx)
{
	struct iflist *ptmp = table->firstvisible;

	wattrset(table->statwin, HIGHATTR);
	do {
		wmove(table->statwin, ptmp->index - idx, 64 * COLS / 80);
		if (unit == KBITS) {
			ptmp->rate =
			    ((float) (ptmp->spanbr * 8 / 1000)) /
			    ((float) (now - starttime));
			wprintw(table->statwin, "%8.2f %s", ptmp->rate, dispmode(unit));
		} else {
			ptmp->rate =
			    ((float) (ptmp->spanbr / 1024)) /
			    ((float) (now - starttime));
			wprintw(table->statwin, "%8.2f %s", ptmp->rate, dispmode(unit));
		}

		if (ptmp->rate > ptmp->peakrate)
			ptmp->peakrate = ptmp->rate;

		ptmp->spanbr = 0;
		ptmp = ptmp->next_entry;
	} while (ptmp != table->lastvisible->next_entry);
}

void printifentry(struct iflist *ptmp, WINDOW * win, unsigned int idx)
{
	unsigned int target_row;

	if ((ptmp->index < idx) || (ptmp->index > idx + (LINES - 5)))
		return;

	target_row = ptmp->index - idx;

	wattrset(win, STDATTR);
	wmove(win, target_row, 1);
	wprintw(win, "%s", ptmp->ifname);
	wattrset(win, HIGHATTR);
	wmove(win, target_row, 14 * COLS / 80);
	printlargenum(ptmp->total, win);
	wmove(win, target_row, 24 * COLS / 80);
	printlargenum(ptmp->iptotal, win);
	wmove(win, target_row, 34 * COLS / 80);
	printlargenum(ptmp->ip6total, win);
	wmove(win, target_row, 44 * COLS / 80);
	printlargenum(ptmp->noniptotal, win);
	wmove(win, target_row, 53 * COLS / 80);
	wprintw(win, "%7lu", ptmp->badtotal);
}

void preparescreen(struct iftab *table)
{
	struct iflist *ptmp = table->head;
	unsigned int i = 1;

	unsigned int winht = LINES - 4;

	table->firstvisible = table->head;

	do {
		printifentry(ptmp, table->statwin, 1);

		if (i <= winht)
			table->lastvisible = ptmp;

		ptmp = ptmp->next_entry;
		i++;
	} while ((ptmp != NULL) && (i <= winht));
}

void labelstats(WINDOW * win)
{
	wmove(win, 0, 1);
	wprintw(win, " Iface ");
	/* 14, 24, 34, ... from printifentry() */
	/* 10 = strlen(printed number); from printlargenum() */
	/* 7 = strlen(" Total ") */
	/* 1 = align the string on 'l' from " Total " */
	wmove(win, 0, (14 * COLS / 80) + 10 - 7 + 1);
	wprintw(win, " Total ");
	wmove(win, 0, (24 * COLS / 80) + 10 - 6 + 1);
	wprintw(win, " IPv4 ");
	wmove(win, 0, (34 * COLS / 80) + 10 - 6 + 1);
	wprintw(win, " IPv6 ");
	wmove(win, 0, (44 * COLS / 80) + 10 - 7 + 1);
	wprintw(win, " NonIP ");
	wmove(win, 0, (53 * COLS / 80) + 8 - 7 + 1);
	wprintw(win, " BadIP ");
	wmove(win, 0, (64 * COLS / 80) + 14 - 10);
	wprintw(win, " Activity ");
}

void initiftab(struct iftab *table)
{
	table->borderwin = newwin(LINES - 2, COLS, 1, 0);
	table->borderpanel = new_panel(table->borderwin);

	move(LINES - 1, 1);
	scrollkeyhelp();
	stdexitkeyhelp();
	wattrset(table->borderwin, BOXATTR);
	tx_box(table->borderwin, ACS_VLINE, ACS_HLINE);
	labelstats(table->borderwin);
	table->statwin = newwin(LINES - 4, COLS - 2, 2, 1);
	table->statpanel = new_panel(table->statwin);
	tx_stdwinset(table->statwin);
	wtimeout(table->statwin, -1);
	wattrset(table->statwin, STDATTR);
	tx_colorwin(table->statwin);
	wattrset(table->statwin, BOXATTR);
	wmove(table->borderwin, LINES - 3, 32 * COLS / 80);
	wprintw(table->borderwin,
		" Total, IP, NonIP, and BadIP are packet counts ");
}

/*
 * Scrolling routines for the general interface statistics window
 */

void scrollgstatwin(struct iftab *table, int direction, unsigned int *idx)
{
	char buf[255];

	sprintf(buf, "%%%dc", COLS - 2);
	wattrset(table->statwin, STDATTR);
	if (direction == SCROLLUP) {
		if (table->lastvisible->next_entry != NULL) {
			wscrl(table->statwin, 1);
			table->lastvisible = table->lastvisible->next_entry;
			table->firstvisible = table->firstvisible->next_entry;
			(*idx)++;
			wmove(table->statwin, LINES - 5, 0);
			scrollok(table->statwin, 0);
			wprintw(table->statwin, buf, ' ');
			scrollok(table->statwin, 1);
			printifentry(table->lastvisible, table->statwin, *idx);
		}
	} else {
		if (table->firstvisible != table->head) {
			wscrl(table->statwin, -1);
			table->firstvisible = table->firstvisible->prev_entry;
			table->lastvisible = table->lastvisible->prev_entry;
			(*idx)--;
			wmove(table->statwin, 0, 0);
			wprintw(table->statwin, buf, ' ');
			printifentry(table->firstvisible, table->statwin, *idx);
		}
	}
}

void pagegstatwin(struct iftab *table, int direction, unsigned int *idx)
{
	int i = 1;

	if (direction == SCROLLUP) {
		while ((i <= LINES - 5)
		       && (table->lastvisible->next_entry != NULL)) {
			i++;
			scrollgstatwin(table, direction, idx);
		}
	} else {
		while ((i <= LINES - 5) && (table->firstvisible != table->head)) {
			i++;
			scrollgstatwin(table, direction, idx);
		}
	}
}


/*
 * The general interface statistics function
 */

void ifstats(const struct OPTIONS *options, struct filterstate *ofilter,
	     time_t facilitytime)
{
	int logging = options->logging;
	struct iftab table;

	char buf[MAX_PACKET_SIZE];
	char *packet;
	int pkt_result = 0;

	struct sockaddr_ll fromaddr;

	struct iflist *ptmp = NULL;

	unsigned int idx = 1;

	FILE *logfile = NULL;

	int br;

	int ch;

	int fd;

	struct timeval tv;
	time_t starttime = 0;
	time_t statbegin = 0;
	time_t now = 0;
	unsigned long long unow = 0;
	time_t startlog = 0;
	time_t updtime = 0;
	unsigned long long updtime_usec = 0;

	struct promisc_states *promisc_list;

	if (!facility_active(GSTATIDFILE, ""))
		mark_facility(GSTATIDFILE, "general interface statistics", "");
	else {
		write_error("General interface stats already active in another process");
		return;
	}

	initiflist(&(table.head));
	if (table.head == NULL) {
		no_ifaces_error();
		unmark_facility(GSTATIDFILE, "");
		return;
	}

	initiftab(&table);

	if ((first_active_facility()) && (options->promisc)) {
		init_promisc_list(&promisc_list);
		save_promisc_list(promisc_list);
		srpromisc(1, promisc_list);
		destroy_promisc_list(&promisc_list);
	}

	adjust_instance_count(PROCCOUNTFILE, 1);

	if (logging) {
		if (strcmp(current_logfile, "") == 0) {
			strcpy(current_logfile, GSTATLOG);

			if (!daemonized)
				input_logfile(current_logfile, &logging);
		}
	}

	if (logging) {
		opentlog(&logfile, GSTATLOG);

		if (logfile == NULL)
			logging = 0;
	}
	if (logging)
		signal(SIGUSR1, rotate_gstat_log);

	rotate_flag = 0;
	writelog(logging, logfile,
		 "******** General interface statistics started ********");

	preparescreen(&table);

	update_panels();
	doupdate();

	fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if(fd == -1) {
		write_error("Unable to obtain monitoring socket");
		goto err;
	}

	//isdnfd = -1;
	exitloop = 0;
	gettimeofday(&tv, NULL);
	starttime = startlog = statbegin = tv.tv_sec;

	while (!exitloop) {
		gettimeofday(&tv, NULL);
		now = tv.tv_sec;
		unow = tv.tv_sec * 1000000ULL + tv.tv_usec;

		if ((now - starttime) >= 5) {
			updaterates(&table, options->actmode, starttime,
				    now, idx);
			printelapsedtime(statbegin, now, LINES - 3, 1,
					 table.borderwin);
			starttime = now;
		}
		if (((now - startlog) >= options->logspan) && (logging)) {
			writegstatlog(&table, options->actmode,
				      time((time_t *) NULL) - statbegin,
				      logfile);
			startlog = now;
		}
		if (((options->updrate != 0)
		     && (now - updtime >= options->updrate))
		    || ((options->updrate == 0)
			&& (unow - updtime_usec >= DEFAULT_UPDATE_DELAY))) {
			update_panels();
			doupdate();
			updtime = now;
			updtime_usec = unow;
		}
		check_rotate_flag(&logfile, logging);

		if ((facilitytime != 0)
		    && (((now - statbegin) / 60) >= facilitytime))
			exitloop = 1;

		getpacket(fd, buf, &fromaddr, &ch, &br, table.statwin);

		switch (ch) {
		case ERR:
			/* no key ready, do nothing */
			break;
		case KEY_UP:
			scrollgstatwin(&table, SCROLLDOWN, &idx);
			break;
		case KEY_DOWN:
			scrollgstatwin(&table, SCROLLUP, &idx);
			break;
		case KEY_PPAGE:
		case '-':
			pagegstatwin(&table, SCROLLDOWN, &idx);
			break;
		case KEY_NPAGE:
		case ' ':
			pagegstatwin(&table, SCROLLUP, &idx);
			break;
		case 12:
		case 'l':
		case 'L':
			tx_refresh_screen();
			break;
		case 'Q':
		case 'q':
		case 'X':
		case 'x':
		case 27:
		case 24:
			exitloop = 1;
			break;
		}
		if (br <= 0)
			continue;

		pkt_result = processpacket(buf, &packet, (unsigned int *) &br,
					   NULL, NULL, NULL, &fromaddr,
					   ofilter,
					   MATCH_OPPOSITE_USECONFIG,
					   options->v6inv4asv6);

		if (pkt_result != PACKET_OK
		    && pkt_result != MORE_FRAGMENTS)
			continue;

		ptmp = positionptr(table.head, fromaddr.sll_ifindex);
		if (!ptmp)
			continue;

		ptmp->total++;

		ptmp->spanbr += br;
		ptmp->br += br;

		if (fromaddr.sll_protocol == ETH_P_IP) {
			ptmp->iptotal++;

			if (pkt_result == CHECKSUM_ERROR) {
				(ptmp->badtotal)++;
				continue;
			}
		} else if (fromaddr.sll_protocol == ETH_P_IPV6) {
			ptmp->ip6total++;
		} else {
			(ptmp->noniptotal)++;
		}
		printifentry(ptmp, table.statwin, idx);
	}
	close(fd);

err:
	if ((options->promisc) && (is_last_instance())) {
		load_promisc_list(&promisc_list);
		srpromisc(0, promisc_list);
		destroy_promisc_list(&promisc_list);
	}

	adjust_instance_count(PROCCOUNTFILE, -1);

	del_panel(table.statpanel);
	delwin(table.statwin);
	del_panel(table.borderpanel);
	delwin(table.borderwin);
	update_panels();
	doupdate();

	if (logging) {
		signal(SIGUSR1, SIG_DFL);
		writegstatlog(&table, options->actmode,
			      time((time_t *) NULL) - statbegin, logfile);
		writelog(logging, logfile,
			 "******** General interface statistics stopped ********");
		fclose(logfile);
	}
	destroyiflist(table.head);
	pkt_cleanup();
	unmark_facility(GSTATIDFILE, "");
	strcpy(current_logfile, "");
}

void selectiface(char *ifname, int withall, int *aborted)
{
	int ch;

	struct iflist *list;
	struct iflist *ptmp;

	struct scroll_list scrolllist;

	initiflist(&list);

	if (list == NULL) {
		no_ifaces_error();
		*aborted = 1;
		return;
	}

	if ((withall) && (list != NULL)) {
		ptmp = xmalloc(sizeof(struct iflist));
		strncpy(ptmp->ifname, "All interfaces", sizeof(ptmp->ifname));
		ptmp->ifindex = 0;

		ptmp->prev_entry = NULL;
		list->prev_entry = ptmp;
		ptmp->next_entry = list;
		list = ptmp;
	}
	tx_listkeyhelp(STDATTR, HIGHATTR);

	ptmp = list;

	tx_init_listbox(&scrolllist, 24, 14, (COLS - 24) / 2 - 9,
			(LINES - 14) / 2, STDATTR, BOXATTR, BARSTDATTR,
			HIGHATTR);

	tx_set_listbox_title(&scrolllist, "Select Interface", 1);

	while (ptmp != NULL) {
		tx_add_list_entry(&scrolllist, (char *) ptmp, ptmp->ifname);
		ptmp = ptmp->next_entry;
	}

	tx_show_listbox(&scrolllist);
	tx_operate_listbox(&scrolllist, &ch, aborted);
	tx_close_listbox(&scrolllist);

	if (!(*aborted) && (list != NULL)) {
		ptmp = (struct iflist *) scrolllist.textptr->nodeptr;
		if ((withall) && (ptmp->prev_entry == NULL))	/* All Interfaces */
			strcpy(ifname, "");
		else
			strcpy(ifname, ptmp->ifname);
	}

	tx_destroy_list(&scrolllist);
	destroyiflist(list);
	update_panels();
	doupdate();
}
