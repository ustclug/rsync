/* 
   Copyright (C) Andrew Tridgell 1996
   Copyright (C) Paul Mackerras 1996
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* handle the mapping of uid/gid and user/group names between systems.
   If the source username/group does not exist on the target then use
   the numeric ids. Never do any mapping for uid=0 or gid=0 as these
   are special.
*/

#include "rsync.h"

#ifdef HAVE_GETGROUPS
# if !defined(GETGROUPS_T)
#  define GETGROUPS_T gid_t
# endif
# ifndef NGROUPS_MAX
/* It ought to be defined, but just in case. */
#  define NGROUPS_MAX 32
# endif
#endif

extern int verbose;
extern int preserve_uid;
extern int preserve_gid;
extern int numeric_ids;
extern int am_root;

struct idlist {
	struct idlist *next;
	int id, id2;
	char *name;
};

static struct idlist *uidlist;
static struct idlist *gidlist;

static struct idlist *add_list(int id, char *name)
{
	struct idlist *list = new(struct idlist);
	if (!list) out_of_memory("add_list");
	list->next = NULL;
	list->name = strdup(name);
	if (!list->name) out_of_memory("add_list");
	list->id = (int)id;
	return list;
}



/* turn a uid into a user name */
static char *uid_to_name(uid_t uid)
{
	struct passwd *pass = getpwuid(uid);
	if (pass) return(pass->pw_name);
	return NULL;
}

/* turn a gid into a group name */
static char *gid_to_name(gid_t gid)
{
	struct group *grp = getgrgid(gid);
	if (grp) return(grp->gr_name);
	return NULL;
}

static int map_uid(int id, char *name)
{
	uid_t uid;
	if (uid != 0 && name_to_uid(name, &uid))
		return uid;
	return id;
}

static int map_gid(int id, char *name)
{
	gid_t gid;
	if (gid != 0 && name_to_gid(name, &gid))
		return gid;
	return id;
}

/* this function is a definate candidate for a faster algorithm */
static uid_t match_uid(uid_t uid)
{
	static uid_t last_in, last_out;
	struct idlist *list = uidlist;

	if (uid == last_in)
		return last_out;

	last_in = uid;

	while (list) {
		if (list->id == (int)uid) {
			last_out = (uid_t)list->id2;
			return last_out;
		}
		list = list->next;
	}
	
	last_out = uid;
	return last_out;
}

static int is_in_group(gid_t gid)
{
#ifdef HAVE_GETGROUPS
	static gid_t last_in = GID_NONE, last_out;
	static int ngroups = -2;
	static GETGROUPS_T *gidset;
	int n;

	if (gid == last_in)
		return last_out;
	if (ngroups < -1) {
		gid_t mygid = MY_GID();
		if ((ngroups = getgroups(0, 0)) < 0)
			ngroups = 0;
		gidset = new_array(GETGROUPS_T, ngroups+1);
		if (ngroups > 0)
			ngroups = getgroups(ngroups, gidset);
		/* The default gid might not be in the list on some systems. */
		for (n = 0; n < ngroups; n++) {
			if (gidset[n] == mygid)
				break;
		}
		if (n == ngroups)
			gidset[ngroups++] = mygid;
		if (verbose > 3) {
			char gidbuf[NGROUPS_MAX*16+32];
			int pos;
			sprintf(gidbuf, "process has %d gid%s: ",
			    ngroups, ngroups == 1? "" : "s");
			pos = strlen(gidbuf);
			for (n = 0; n < ngroups; n++) {
				sprintf(gidbuf+pos, " %ld", (long)gidset[n]);
				pos += strlen(gidbuf+pos);
			}
			rprintf(FINFO, "%s\n", gidbuf);
		}
	}

	last_in = gid;
	for (n = 0; n < ngroups; n++) {
		if (gidset[n] == gid)
			return last_out = 1;
	}
	return last_out = 0;

#else
	static gid_t mygid = GID_NONE;
	if (mygid == GID_NONE) {
		mygid = MY_GID();
		if (verbose > 3)
			rprintf(FINFO, "process has gid %ld\n", (long)mygid);
	}
	return gid == mygid;
#endif
}

static gid_t match_gid(gid_t gid)
{
	static gid_t last_in = GID_NONE, last_out = GID_NONE;
	struct idlist *list = gidlist;

	if (gid == last_in)
		return last_out;

	last_in = gid;

	while (list) {
		if (list->id == (int)gid) {
			last_out = (gid_t)list->id2;
			return last_out;
		}
		list = list->next;
	}
	
	if (am_root || is_in_group(gid))
		last_out = gid;
	else
		last_out = GID_NONE;
	return last_out;
}

/* add a uid to the list of uids */
void add_uid(uid_t uid)
{
	struct idlist *list = uidlist;
	char *name;

	if (numeric_ids) return;

	/* don't map root */
	if (uid==0) return;

	if (!list) {
		if (!(name = uid_to_name(uid))) return;
		uidlist = add_list((int)uid, name);
		return;
	}

	while (list->next) {
		if (list->id == (int)uid) return;
		list = list->next;
	}

	if (list->id == (int)uid) return;

	if (!(name = uid_to_name(uid))) return;

	list->next = add_list((int)uid, name);
}

/* add a gid to the list of gids */
void add_gid(gid_t gid)
{
	struct idlist *list = gidlist;
	char *name;

	if (numeric_ids) return;

	/* don't map root */
	if (gid==0) return;

	if (!list) {
		if (!(name = gid_to_name(gid))) return;
		gidlist = add_list((int)gid, name);
		return;
	}

	while (list->next) {
		if (list->id == (int)gid) return;
		list = list->next;
	}

	if (list->id == (int)gid) return;

	if (!(name = gid_to_name(gid))) return;

	list->next = add_list((int)gid, name);
}


/* send a complete uid/gid mapping to the peer */
void send_uid_list(int f)
{
	struct idlist *list;

	if (numeric_ids) return;

	if (preserve_uid) {
		/* we send sequences of uid/byte-length/name */
		list = uidlist;
		while (list) {
			int len = strlen(list->name);
			write_int(f, list->id);
			write_byte(f, len);
			write_buf(f, list->name, len);
			list = list->next;
		}

		/* terminate the uid list with a 0 uid. We explicitly exclude
		 * 0 from the list */
		write_int(f, 0);
	}

	if (preserve_gid) {
		list = gidlist;
		while (list) {
			int len = strlen(list->name);
			write_int(f, list->id);
			write_byte(f, len);
			write_buf(f, list->name, len);
			list = list->next;
		}
		write_int(f, 0);
	}
}

/* recv a complete uid/gid mapping from the peer and map the uid/gid
 * in the file list to local names */
void recv_uid_list(int f, struct file_list *flist)
{
	int id, i;
	char *name;
	struct idlist *list;

	if (numeric_ids) return;

	if (preserve_uid) {
		/* read the uid list */
		list = uidlist;
		while ((id = read_int(f)) != 0) {
			int len = read_byte(f);
			name = new_array(char, len+1);
			if (!name) out_of_memory("recv_uid_list");
			read_sbuf(f, name, len);
			if (!list) {
				uidlist = add_list(id, name);
				list = uidlist;
			} else {
				list->next = add_list(id, name);
				list = list->next;
			}
			list->id2 = map_uid(id, name);
			free(name);
		}
		if (verbose > 3) {
			for (list = uidlist; list; list = list->next) {
				rprintf(FINFO, "uid %ld (%s) maps to %ld\n",
				    (long)list->id, list->name,
				    (long)list->id2);
			}
		}
	}


	if (preserve_gid) {
		/* and the gid list */
		list = gidlist;
		while ((id = read_int(f)) != 0) {
			int len = read_byte(f);
			name = new_array(char, len+1);
			if (!name) out_of_memory("recv_uid_list");
			read_sbuf(f, name, len);
			if (!list) {
				gidlist = add_list(id, name);
				list = gidlist;
			} else {
				list->next = add_list(id, name);
				list = list->next;
			}
			list->id2 = map_gid(id, name);
			if (!am_root && !is_in_group(list->id2))
				list->id2 = GID_NONE;
			free(name);
		}
		if (verbose > 3) {
			for (list = gidlist; list; list = list->next) {
				rprintf(FINFO, "gid %ld (%s) maps to %ld\n",
				    (long)list->id, list->name,
				    (long)list->id2);
			}
		}
	}

	if (!(am_root && preserve_uid) && !preserve_gid) return;

	/* now convert the uid/gid of all files in the list to the mapped
	 * uid/gid */
	for (i = 0; i < flist->count; i++) {
		if (am_root && preserve_uid && flist->files[i]->uid != 0)
			flist->files[i]->uid = match_uid(flist->files[i]->uid);
		if (preserve_gid && (!am_root || flist->files[i]->gid != 0))
			flist->files[i]->gid = match_gid(flist->files[i]->gid);
	}
}
