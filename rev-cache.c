#include "cache.h"
#include "object.h"
#include "commit.h"
#include "tree.h"
#include "tree-walk.h"
#include "blob.h"
#include "tag.h"
#include "diff.h"
#include "revision.h"
#include "rev-cache.h"
#include "run-command.h"
#include "string-list.h"

struct cache_slice_pointer {
	char signature[8]; /* REVCOPTR */
	char version;
	char path[PATH_MAX + 1];
};

/* list resembles pack index format */
static uint32_t fanout[0xff + 2];

static unsigned char *idx_map;
static int idx_size;
static struct rc_index_header idx_head;
static unsigned char *idx_caches;
static char no_idx;

static struct strbuf *g_buffer;

#define PATH_WIDTH	RC_PATH_WIDTH
#define PATH_SIZE(x)	RC_PATH_SIZE(x)

#define OE_SIZE		RC_OE_SIZE
#define IE_SIZE		RC_IE_SIZE

#define OE_CAST(p)	RC_OE_CAST(p)
#define IE_CAST(p)	RC_IE_CAST(p)

#define ACTUAL_OBJECT_ENTRY_SIZE(e)		RC_ACTUAL_OBJECT_ENTRY_SIZE(e)
#define ENTRY_SIZE_OFFSET(e)			RC_ENTRY_SIZE_OFFSET(e)

#define SLOP		5

/* initialization */

static int get_index_head(unsigned char *map, int len, struct rc_index_header *head, uint32_t *fanout, unsigned char **caches)
{
	struct rc_index_header whead;
	int i, index = sizeof(struct rc_index_header);
	
	memcpy(&whead, map, sizeof(struct rc_index_header));
	if (memcmp(whead.signature, "REVINDEX", 8) || whead.version > SUPPORTED_REVINDEX_VERSION)
		return -1;
	
	memcpy(head->signature, "REVINDEX", 8);
	head->version = whead.version;
	head->ofs_objects = ntohl(whead.ofs_objects);
	head->object_nr = ntohl(whead.object_nr);
	head->cache_nr = whead.cache_nr;
	head->max_date = ntohl(whead.max_date);
	
	if (len < index + head->cache_nr * 20 + 0x100 * sizeof(uint32_t))
		return -2;
	
	*caches = xmalloc(head->cache_nr * 20);
	memcpy(*caches, map + index, head->cache_nr * 20);
	index += head->cache_nr * 20;
	
	memcpy(fanout, map + index, 0x100 * sizeof(uint32_t));
	for (i = 0; i <= 0xff; i++)
		fanout[i] = ntohl(fanout[i]);
	fanout[0x100] = len;
	
	return 0;
}

/* added in init_index */
static void cleanup_cache_slices(void)
{
	if (idx_map) {
		free(idx_caches);
		munmap(idx_map, idx_size);
		idx_map = 0;
	}
	
}

static int init_index(void)
{
	int fd;
	struct stat fi;
	
	fd = open(git_path("rev-cache/index"), O_RDONLY);
	if (fd == -1 || fstat(fd, &fi))
		goto end;
	if (fi.st_size < sizeof(struct rc_index_header))
		goto end;
	
	idx_size = fi.st_size;
	idx_map = xmmap(0, idx_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (idx_map == MAP_FAILED)
		goto end;
	if (get_index_head(idx_map, fi.st_size, &idx_head, fanout, &idx_caches))
		goto end;
	
	atexit(cleanup_cache_slices);
	
	return 0;
	
end:
	idx_map = 0;
	no_idx = 1;
	return -1;
}

/* this assumes index is already loaded */
static struct rc_index_entry *search_index(unsigned char *sha1)
{
	int start, end, starti, endi, i, len, r;
	struct rc_index_entry *ie;
	
	if (!idx_map)
		return 0;
	
	/* binary search */
	start = fanout[(int)sha1[0]];
	end = fanout[(int)sha1[0] + 1];
	len = (end - start) / IE_SIZE;
	if (!len || len * IE_SIZE != end - start)
		return 0;
	
	starti = 0;
	endi = len - 1;
	for (;;) {
		i = (endi + starti) / 2;
		ie = IE_CAST(idx_map + start + i * IE_SIZE);
		r = hashcmp(sha1, ie->sha1);
		
		if (r) {
			if (starti + 1 == endi) {
				starti++;
				continue;
			} else if (starti == endi)
				break;
			
			if (r > 0)
				starti = i;
			else /* r < 0 */
				endi = i;
		} else
			return ie;
	}
	
	return 0;
}

unsigned char *get_cache_slice(struct commit *commit)
{
	struct rc_index_entry *ie;
	
	if (!idx_map) {
		if (no_idx)
			return 0;
		init_index();
	}
	
	if (commit->date > idx_head.max_date)
		return 0;
	
	ie = search_index(commit->object.sha1);
	if (ie && ie->cache_index < idx_head.cache_nr)
		return idx_caches + ie->cache_index * 20;
	
	return 0;
}


/* traversal */

static unsigned long decode_size(unsigned char *str, int len);

static void handle_noncommit(struct rev_info *revs, struct commit *commit, struct rc_object_entry *entry)
{
	struct blob *blob;
	struct tree *tree;
	struct object *obj;
	unsigned long size;
	
	size = decode_size((unsigned char *)entry + ENTRY_SIZE_OFFSET(entry), entry->size_size);
	switch (entry->type) {
	case OBJ_TREE : 
		if (!revs->tree_objects)
			return;
		
		tree = lookup_tree(entry->sha1);
		if (!tree)
			return;
		
		tree->size = size;
		commit->tree = tree;
		obj = (struct object *)tree;
		break;
	
	case OBJ_BLOB : 
		if (!revs->blob_objects)
			return;
		
		blob = lookup_blob(entry->sha1);
		if (!blob)
			return;
		
		obj = (struct object *)blob;
		break;
		
	default : 
		/* tag objects aren't really supposed to be here */
		return;
	}
	
	obj->flags |= FACE_VALUE;
	add_pending_object(revs, obj, "");
}

static int setup_traversal(struct rc_slice_header *head, unsigned char *map, struct commit *commit, struct commit_list **work)
{
	struct rc_index_entry *iep;
	struct rc_object_entry *oep;
	struct commit_list *prev, *wp, **wpp;
	int retval;
	
	iep = search_index(commit->object.sha1);
	oep = OE_CAST(map + ntohl(iep->pos));
	oep->include = 1;
	retval = ntohl(iep->pos);
	
	/* include any others in the work array */
	prev = 0;
	wpp = work;
	wp = *work;
	while (wp) {
		struct object *obj = &wp->item->object;
		struct commit *co;
		int t;
		
		/* is this in our cache slice? */
		iep = search_index(obj->sha1);
		if (!iep || hashcmp(idx_caches + iep->cache_index * 20, head->sha1)) {
			prev = wp;
			wp = wp->next;
			wpp = &wp;
 			continue;
		}
		
		t = ntohl(iep->pos);
		oep = OE_CAST(map + t);
		
		oep->include = 1;
		oep->uninteresting = !!(obj->flags & UNINTERESTING);
		if (t < retval)
			retval = t;
		
		/* remove from work list */
		co = pop_commit(wpp);
		wp = *wpp;
		if (prev)
			prev->next = wp;
	}
	
	return retval;
}

#define IPATH				0x40
#define UPATH				0x80

#define GET_COUNT(x)		((x) & 0x3f)
#define SET_COUNT(x, s)		((x) = ((x) & ~0x3f) | ((s) & 0x3f))

static int traverse_cache_slice_1(struct rc_slice_header *head, unsigned char *map, 
	struct rev_info *revs, struct commit *commit, 
	unsigned long *date_so_far, int *slop_so_far, 
	struct commit_list ***queue, struct commit_list **work)
{
	struct commit_list *insert_cache = 0;
	struct commit **last_objects, *co;
	int i, total_path_nr = head->path_nr, retval = -1;
	char consume_children = 0;
	unsigned char *paths;
	
	paths = xcalloc(total_path_nr, PATH_WIDTH);
	last_objects = xcalloc(total_path_nr, sizeof(struct commit *));
	
	i = setup_traversal(head, map, commit, work);
	
	/* i already set */
	while (i < head->size) {
		struct rc_object_entry *entry = OE_CAST(map + i);
		int path = ntohs(entry->path);
		struct object *obj;
		int index = i;
		
		i += ACTUAL_OBJECT_ENTRY_SIZE(entry);
		
		/* add extra objects if necessary */
		if (entry->type != OBJ_COMMIT) {
			if (consume_children)
				handle_noncommit(revs, co, entry);
			
			continue;
		} else
			consume_children = 0;
		
		if (path >= total_path_nr)
			goto end;
		
		/* in one of our branches? 
		 * uninteresting trumps interesting */
		if (entry->include)
			paths[path] |= entry->uninteresting ? UPATH : IPATH;
		else if (!paths[path])
			continue;
		
		/* date stuff */
		if (revs->max_age != -1 && ntohl(entry->date) < revs->max_age)
			paths[path] |= UPATH;
		
		/* lookup object */
		co = lookup_commit(entry->sha1);
		obj = &co->object;
		
		if (obj->flags & UNINTERESTING)
			paths[path] |= UPATH;
		
		if ((paths[path] & IPATH) && (paths[path] & UPATH)) {
			paths[path] = UPATH;
			
			/* mark edge */
			if (last_objects[path]) {
				parse_commit(last_objects[path]);
				
				/* we needn't worry about the unique field; that will be valid as 
				 * long as we're not a end entry */
				last_objects[path]->object.flags &= ~FACE_VALUE;
				last_objects[path] = 0;
			}
		}
		
		/* now we gotta re-assess the whole interesting thing... */
		entry->uninteresting = !!(paths[path] & UPATH);
		
		/* first close paths */
		if (entry->split_nr) {
			int j, off = index + OE_SIZE + PATH_SIZE(entry->merge_nr);
			
			for (j = 0; j < entry->split_nr; j++) {
				unsigned short p = ntohs(*(unsigned short *)(map + off + PATH_SIZE(j)));
				
				if (p >= total_path_nr)
					goto end;
				
				/* boundary commit? */
				if ((paths[p] & IPATH) && entry->uninteresting) {
					if (last_objects[p]) {
						parse_commit(last_objects[p]);
						
						last_objects[p]->object.flags &= ~FACE_VALUE;
						last_objects[p] = 0;
					}
				} else if (last_objects[p] && !last_objects[p]->object.parsed)
					commit_list_insert(co, &last_objects[p]->parents);
				
				/* can't close a merge path until all are parents have been encountered */
				if (GET_COUNT(paths[p])) {
					SET_COUNT(paths[p], GET_COUNT(paths[p]) - 1);
					
					if (GET_COUNT(paths[p]))
						continue;
				}
				
				paths[p] = 0;
				last_objects[p] = 0;
			}
		}
		
		/* make topo relations */
		if (last_objects[path] && !last_objects[path]->object.parsed)
			commit_list_insert(co, &last_objects[path]->parents);
		
		/* initialize commit */
		if (!entry->is_end) {
			co->date = ntohl(entry->date);
 			obj->flags |= ADDED | FACE_VALUE;
		} else
			parse_commit(co);
		
		obj->flags |= SEEN;
 		
 		if (entry->uninteresting)
 			obj->flags |= UNINTERESTING;
		
		/* we need to know what the edges are */
		last_objects[path] = co;
		
		/* add to list */
		if (!(obj->flags & UNINTERESTING) || revs->show_all) {
			if (entry->is_end)
				insert_by_date_cached(co, work, insert_cache, &insert_cache);
			else
				*queue = &commit_list_insert(co, *queue)->next;
			
			/* add children to list as well */
			if (obj->flags & UNINTERESTING)
				consume_children = 0;
			else 
				consume_children = 1;
		}
		
		/* open parents */
		if (entry->merge_nr) {
			int j, off = index + OE_SIZE;
			char flag = entry->uninteresting ? UPATH : IPATH;
			
			for (j = 0; j < entry->merge_nr; j++) {
				unsigned short p = ntohs(*(unsigned short *)(map + off + PATH_SIZE(j)));
				
				if (p >= total_path_nr)
					goto end;
				
				if (paths[p] & flag)
					continue;
				
				paths[p] |= flag;
			}
			
			/* make sure we don't use this path before all our parents have had their say */
			SET_COUNT(paths[path], entry->merge_nr);
		}
		
	}
	
	retval = 0;
	
end:
	free(paths);
	free(last_objects);
	
	return retval;
}

static int get_cache_slice_header(unsigned char *cache_sha1, unsigned char *map, int len, struct rc_slice_header *head)
{
	int t;
	
	memcpy(head, map, sizeof(struct rc_slice_header));
	head->ofs_objects = ntohl(head->ofs_objects);
	head->object_nr = ntohl(head->object_nr);
	head->size = ntohl(head->size);
	head->path_nr = ntohs(head->path_nr);
	
	if (memcmp(head->signature, "REVCACHE", 8))
		return -1;
	if (head->version > SUPPORTED_REVCACHE_VERSION)
		return -2;
	if (hashcmp(head->sha1, cache_sha1))
		return -3;
	t = sizeof(struct rc_slice_header);
	if (t != head->ofs_objects || t >= len)
		return -4;
	
	head->size = len;
	
	return 0;
}

static int open_cache_slice(unsigned char *sha1, int flags)
{
	int fd;
	char signature[8];
	
	fd = open(git_path("rev-cache/%s", sha1_to_hex(sha1)), flags);
	if (fd <= 0)
		goto end;
	
	if (read(fd, signature, 8) != 8)
		goto end;
	
	/* a normal revision slice */
	if (!memcmp(signature, "REVCACHE", 8)) {
		lseek(fd, 0, SEEK_SET);
		return fd;
	}
	
	/* slice pointer */
	if (!memcmp(signature, "REVCOPTR", 8)) {
		struct cache_slice_pointer ptr;
		
		lseek(fd, 0, SEEK_SET);
		if (read_in_full(fd, &ptr, sizeof(ptr)) != sizeof(ptr))
			goto end;
		
		if (ptr.version > SUPPORTED_REVCOPTR_VERSION)
			goto end;
		
		close(fd);
		fd = open(ptr.path, flags);
		
		return fd;
	}
	
end:
	if (fd > 0)
		close(fd);
	
	return -1;
}

int traverse_cache_slice(struct rev_info *revs, 
	unsigned char *cache_sha1, struct commit *commit, 
	unsigned long *date_so_far, int *slop_so_far, 
	struct commit_list ***queue, struct commit_list **work)
{
	int fd = -1, retval = -3;
	struct stat fi;
	struct rc_slice_header head;
	struct rev_cache_info *rci;
	unsigned char *map = MAP_FAILED;
	
	/* the index should've been loaded already to find cache_sha1, but it's good 
	 * to be absolutely sure... */
	if (!idx_map)
		init_index();
	if (!idx_map)
		return -1;
	
	/* load options */
	rci = &revs->rev_cache_info;
	
	memset(&head, 0, sizeof(struct rc_slice_header));
	
	fd = open_cache_slice(cache_sha1, O_RDONLY);
	if (fd == -1)
		goto end;
	if (fstat(fd, &fi) || fi.st_size < sizeof(struct rc_slice_header))
		goto end;
	
	map = xmmap(0, fi.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		goto end;
	if (get_cache_slice_header(cache_sha1, map, fi.st_size, &head))
		goto end;
	
	retval = traverse_cache_slice_1(&head, map, revs, commit, date_so_far, slop_so_far, queue, work);
	
end:
	if (map != MAP_FAILED)
		munmap(map, fi.st_size);
	if (fd != -1)
		close(fd);
	
	return retval;
}



/* generation */

static int is_endpoint(struct commit *commit)
{
	struct commit_list *list = commit->parents;
	
	while (list) {
		if (!(list->item->object.flags & UNINTERESTING))
			return 0;
		
		list = list->next;
	}
	
	return 1;
}

/* ensures branch is self-contained: parents are either all interesting or all uninteresting */
static void make_legs(struct rev_info *revs)
{
	struct commit_list *list, **plist;
	int total = 0;
	
	/* attach plist to end of commits list */
	list = revs->commits;
	while (list && list->next)
		list = list->next;
	
	if (list)
		plist = &list->next;
	else
		return;
	
	/* duplicates don't matter, as get_revision() ignores them */
	for (list = revs->commits; list; list = list->next) {
		struct commit *item = list->item;
		struct commit_list *parents = item->parents;
		
		if (item->object.flags & UNINTERESTING)
			continue;
		if (is_endpoint(item))
			continue;
		
		while (parents) {
			struct commit *p = parents->item;
			parents = parents->next;
			
			if (!(p->object.flags & UNINTERESTING))
				continue;
			
			p->object.flags &= ~UNINTERESTING;
			parse_commit(p);
			plist = &commit_list_insert(p, plist)->next;
			
			if (!(p->object.flags & SEEN))
				total++;
		}
	}
	
	if (total)
		sort_in_topological_order(&revs->commits, 1);
	
}


struct path_track {
	struct commit *commit;
	int path; /* for keeping track of children */
	
	struct path_track *next, *prev;
};

static unsigned char *paths;
static int path_nr = 1, path_sz;

static struct path_track *path_track;
static struct path_track *path_track_alloc;

#define PATH_IN_USE			0x80 /* biggest bit we can get as a char */

static int get_new_path(void)
{
	int i;
	
	for (i = 1; i < path_nr; i++)
		if (!paths[i])
			break;
	
	if (i == path_nr) {
		if (path_nr >= path_sz) {
			path_sz += 50;
			paths = xrealloc(paths, path_sz);
			memset(paths + path_sz - 50, 0, 50);
		}
		path_nr++;
	}
	
	paths[i] = PATH_IN_USE;
	return i;
}

static void remove_path_track(struct path_track **ppt, char total_free)
{
	struct path_track *t = *ppt;
	
	if (t->next)
		t->next->prev = t->prev;
	if (t->prev)
		t->prev->next = t->next;
	
	t = t->next;
	
	if (total_free)
		free(*ppt);
	else {
		(*ppt)->next = path_track_alloc;
		path_track_alloc = *ppt;	
	}
	
	*ppt = t;
}

static struct path_track *make_path_track(struct path_track **head, struct commit *commit)
{
	struct path_track *pt;
	
	if (path_track_alloc) {
		pt = path_track_alloc;
		path_track_alloc = pt->next;
	} else
		pt = xmalloc(sizeof(struct path_track));
	
	memset(pt, 0, sizeof(struct path_track));
	pt->commit = commit;
	
	pt->next = *head;
	if (*head)
		(*head)->prev = pt;
	*head = pt;
	
	return pt;
}

static void add_path_to_track(struct commit *commit, int path)
{
	make_path_track(&path_track, commit);
	path_track->path = path;
}

static void handle_paths(struct commit *commit, struct rc_object_entry *object, struct strbuf *merge_str, struct strbuf *split_str)
{
	int child_nr, parent_nr, open_parent_nr, this_path;
	struct commit_list *list;
	struct commit *first_parent;
	struct path_track **ppt, *pt;
	
	/* we can only re-use a closed path once all it's children have been encountered, 
	 * as we need to keep track of commit boundaries */
	ppt = &path_track;
	pt = *ppt;
	child_nr = 0;
	while (pt) {
		if (pt->commit == commit) {
			uint16_t write_path;
			
			if (paths[pt->path] != PATH_IN_USE)
				paths[pt->path]--;
			
			/* make sure we can handle this */
			child_nr++;
			if (child_nr > 0x7f)
				die("%s: too many branches!  rev-cache can only handle %d parents/children per commit", 
					sha1_to_hex(object->sha1), 0x7f);
			
			/* add to split list */
			object->split_nr++;
			write_path = htons((unsigned short)pt->path);;
			strbuf_add(split_str, &write_path, PATH_WIDTH);
			
			remove_path_track(ppt, 0);
			pt = *ppt;
		} else {
			pt = pt->next;
			ppt = &pt;
		}
	}
	
	/* initialize our self! */
	if (!commit->indegree) {
		commit->indegree = get_new_path();
		object->is_start = 1;
	}
	
	this_path = commit->indegree;
	paths[this_path] = PATH_IN_USE;
	object->path = htons(this_path);
	
	/* count interesting parents */
	parent_nr = open_parent_nr = 0;
	first_parent = 0;
	for (list = commit->parents; list; list = list->next) {
		if (list->item->object.flags & UNINTERESTING) {
			object->is_end = 1;
			continue;
		}
		
		parent_nr++;
		if (!list->item->indegree)
			open_parent_nr++;
		if (!first_parent)
			first_parent = list->item;
	}
	
	if (!parent_nr)
		return;
	
	if (parent_nr == 1 && open_parent_nr == 1) {
		first_parent->indegree = this_path;
		return;
	}
	
	/* bail out on obscene parent/child #s */
	if (parent_nr > 0x7f)
		die("%s: too many parents in merge!  rev-cache can only handle %d parents/children per commit", 
			sha1_to_hex(object->sha1), 0x7f);
	
	/* make merge list */
	object->merge_nr = parent_nr;
	paths[this_path] = parent_nr;
	
	for (list = commit->parents; list; list = list->next) {
		struct commit *p = list->item;
		uint16_t write_path;
		
		if (p->object.flags & UNINTERESTING)
			continue;
		
		/* unfortunately due to boundary tracking we can't re-use merge paths
		 * (unable to guarantee last parent path = this -> last won't always be able to 
		 * set this as a boundary object */
		if (!p->indegree)
			p->indegree = get_new_path();
		
		write_path = htons((unsigned short)p->indegree);
		strbuf_add(merge_str, &write_path, PATH_WIDTH);
		
		/* make sure path is properly ended */
		add_path_to_track(p, this_path);
	}
	
}


static int encode_size(unsigned long size, unsigned char *out)
{
	int len = 0;
	
	while (size) {
		*out++ = (unsigned char)(size & 0xff);
		size >>= 8;
		len++;
	}
	
	return len;
}

static unsigned long decode_size(unsigned char *str, int len)
{
	unsigned long size = 0;
	int shift = 0;
	
	while (len--) {
		size |= (unsigned long)*str << shift;
		shift += 8;
		str++;
	}
	
	return size;
}

static void add_object_entry(const unsigned char *sha1, struct rc_object_entry *entryp, 
	struct strbuf *merge_str, struct strbuf *split_str)
{
	struct rc_object_entry entry;
	unsigned char size_str[7];
	unsigned long size;
	enum object_type type;
	void *data;
	
	if (entryp)
		sha1 = entryp->sha1;
	
	/* retrieve size data */
	data = read_sha1_file(sha1, &type, &size);
	
	if (data)
		free(data);
	
	/* initialize! */
	if (!entryp) {
		memset(&entry, 0, sizeof(entry));
		hashcpy(entry.sha1, sha1);
		entry.type = type;
		
		if (merge_str)
			entry.merge_nr = merge_str->len / PATH_WIDTH;
		if (split_str)
			entry.split_nr = split_str->len / PATH_WIDTH;
		
		entryp = &entry;
	}
	
	entryp->size_size = encode_size(size, size_str);
	
	/* write the muvabitch */
	strbuf_add(g_buffer, entryp, sizeof(entry));
	
	if (merge_str)
		strbuf_add(g_buffer, merge_str->buf, merge_str->len);
	if (split_str)
		strbuf_add(g_buffer, split_str->buf, split_str->len);
	
	strbuf_add(g_buffer, size_str, entryp->size_size);
}

/* returns non-zero to continue parsing, 0 to skip */
typedef int (*dump_tree_fn)(const unsigned char *, const char *, unsigned int); /* sha1, path, mode */

/* we need to walk the trees by hash, so unfortunately we can't use traverse_trees in tree-walk.c */
static int dump_tree(struct tree *tree, dump_tree_fn fn)
{
	struct tree_desc desc;
	struct name_entry entry;
	struct tree *subtree;
	int r;
	
	if (parse_tree(tree))
		return -1;
	
	init_tree_desc(&desc, tree->buffer, tree->size);
	while (tree_entry(&desc, &entry)) {
		switch (fn(entry.sha1, entry.path, entry.mode)) {
		case 0 :
			goto continue_loop;
		default : 
			break;
		}
		
		if (S_ISDIR(entry.mode)) {
			subtree = lookup_tree(entry.sha1);
			if (!subtree)
				return -2;
			
			if ((r = dump_tree(subtree, fn)) < 0)
				return r;
		}
		
continue_loop:
		continue;
	}
	
	return 0;
}

static int dump_tree_callback(const unsigned char *sha1, const char *path, unsigned int mode)
{
	strbuf_add(g_buffer, sha1, 20);
	
	return 1;
}

static void tree_addremove(struct diff_options *options,
	int whatnow, unsigned mode,
	const unsigned char *sha1,
	const char *concatpath)
{
	if (whatnow != '+')
		return;
	
	strbuf_add(g_buffer, sha1, 20);
}

static void tree_change(struct diff_options *options,
	unsigned old_mode, unsigned new_mode,
	const unsigned char *old_sha1,
	const unsigned char *new_sha1,
	const char *concatpath)
{
	if (!hashcmp(old_sha1, new_sha1))
		return;
	
	strbuf_add(g_buffer, new_sha1, 20);
}

static int add_unique_objects(struct commit *commit)
{
	struct commit_list *list;
	struct strbuf os, ost, *orig_buf;
	struct diff_options opts;
	int i, j, next;
	char is_first = 1;
	
	/* ...no, calculate unique objects */
	strbuf_init(&os, 0);
	strbuf_init(&ost, 0);
	orig_buf = g_buffer;
	
	diff_setup(&opts);
	DIFF_OPT_SET(&opts, RECURSIVE);
	DIFF_OPT_SET(&opts, TREE_IN_RECURSIVE);
	opts.change = tree_change;
	opts.add_remove = tree_addremove;
	
	/* this is only called for non-ends (ie. all parents interesting) */
	for (list = commit->parents; list; list = list->next) {
		if (is_first)
			g_buffer = &os;
		else 
			g_buffer = &ost;
		
		strbuf_setlen(g_buffer, 0);
		diff_tree_sha1(list->item->tree->object.sha1, commit->tree->object.sha1, "", &opts);
		qsort(g_buffer->buf, g_buffer->len / 20, 20, (int (*)(const void *, const void *))hashcmp);
		
		/* take intersection */
		if (!is_first) {
			for (next = i = j = 0; i < os.len; i += 20) {
				while (j < ost.len && hashcmp((unsigned char *)(ost.buf + j), (unsigned char *)(os.buf + i)) < 0)
					j += 20;
				
				if (j >= ost.len || hashcmp((unsigned char *)(ost.buf + j), (unsigned char *)(os.buf + i)))
					continue;
				
				if (next != i)
					memcpy(os.buf + next, os.buf + i, 20);
				next += 20;
			}
			
			if (next != i)
				strbuf_setlen(&os, next);
		} else
			is_first = 0;
	}
	
	/* no parents (!) */
	if (is_first) {
		g_buffer = &os;
		dump_tree(commit->tree, dump_tree_callback);
	}
	
	/* the ordering of non-commit objects dosn't really matter, so we're not gonna bother */
	g_buffer = orig_buf;
	for (i = 0; i < os.len; i += 20)
		add_object_entry((unsigned char *)(os.buf + i), 0, 0, 0);
	
	strbuf_release(&ost);
	strbuf_release(&os);
	
	return i / 20;
}

static int add_objects_verbatim_1(unsigned char *map, int *index)
{
	struct object_entry *entry = OE_CAST(map + *index);
	int i = *index, object_nr = 0;
	
	i += ACTUAL_OBJECT_ENTRY_SIZE(entry);
	for (;;) {
		int pos = i;
		
		entry = OE_CAST(map + i);
		i += ACTUAL_OBJECT_ENTRY_SIZE(entry);
		
		if (entry->type == OBJ_COMMIT) {
			*index = pos;
			return object_nr;
		}
		
		strbuf_add(g_buffer, map + pos, i - pos);
		object_nr++;
	}
	
	return 0;
}

static int add_objects_verbatim(struct rev_cache_info *rci, struct commit *commit)
{
	struct rev_cache_slice_map *map;
	char found = 0;
	struct index_entry *ie;
	struct object_entry *entry;
	int object_nr, i;
	
	if (!rci->maps)
		return -1;
	
	/* check if we can continue where we left off */
	map = rci->last_map;
	if (!map)
		goto search_me;
	
	i = map->last_index;
	entry = OE_CAST(map->map + i);
	if (hashcmp(entry->sha1, commit->object.sha1))
		goto search_me;
	
	found = 1;
	
search_me:
	if (!found) {
		ie = search_index(commit->object.sha1);
		if (!ie)
			return -2;
		
		map = rci->maps + ie->cache_index;
		if (!map->size)
			return -3;
		
		i = ntohl(ie->pos);
		entry = OE_CAST(map->map + i);
		if (entry->type != OBJ_COMMIT || hashcmp(entry->sha1, commit->object.sha1))
			return -4;
	}
	
	/* can't handle end commits */
	if (entry->is_end)
		return -5;
	
	object_nr = add_objects_verbatim_1(map->map, &i);
	
	/* remember this */
	rci->last_map = map;
	map->last_index = i;
	
	return object_nr;
}

static void init_revcache_directory(void)
{
	struct stat fi;
	
	if (stat(git_path("rev-cache"), &fi) || !S_ISDIR(fi.st_mode))
		if (mkdir(git_path("rev-cache"), 0666))
			die("can't make rev-cache directory");
	
}

void init_rev_cache_info(struct rev_cache_info *rci)
{
	memset(rci, 0, sizeof(struct rev_cache_info));
	
	rci->objects = 1;
	rci->legs = 0;
	rci->make_index = 1;
	rci->fuse_me = 0;
	
	rci->overwrite_all = 0;
	
	rci->save_unique = 0;
	rci->add_to_pending = 1;
	
	rci->ignore_size = 0;
}

void maybe_fill_with_defaults(struct rev_cache_info *rci)
{
	static struct rev_cache_info def_rci;
	
	if (rci)
		return;
	
	init_rev_cache_info(&def_rci);
	rci = &def_rci;
}

int make_cache_slice(struct rev_cache_info *rci, 
	struct rev_info *revs, struct commit_list **starts, struct commit_list **ends, 
	unsigned char *cache_sha1)
{
	struct rev_info therevs;
	struct strbuf buffer, startlist, endlist;
	struct rc_slice_header head;
	struct commit *commit;
	unsigned char sha1[20];
	struct strbuf merge_paths, split_paths;
	int object_nr, total_sz, fd;
	char file[PATH_MAX], *newfile;
	struct rev_cache_info *trci;
	git_SHA_CTX ctx;
	
	maybe_fill_with_defaults(rci);
	
	init_revcache_directory();
	strcpy(file, git_path("rev-cache/XXXXXX"));
	fd = xmkstemp(file);
	
	strbuf_init(&buffer, 0);
	strbuf_init(&startlist, 0);
	strbuf_init(&endlist, 0);
	strbuf_init(&merge_paths, 0);
	strbuf_init(&split_paths, 0);
	g_buffer = &buffer;
	
	if (!revs) {
		revs = &therevs;
		init_revisions(revs, 0);
		
		/* we're gonna assume no one else has already traversed this... */
		while ((commit = pop_commit(starts)))
			add_pending_object(revs, &commit->object, 0);
		
		while ((commit = pop_commit(ends))) {
			commit->object.flags |= UNINTERESTING;
			add_pending_object(revs, &commit->object, 0);
		}
	}
	
	/* write head placeholder */
	memset(&head, 0, sizeof(head));
	head.ofs_objects = htonl(sizeof(head));
	xwrite(fd, &head, sizeof(head));
	
	/* init revisions! */
	revs->tree_objects = 1;
	revs->blob_objects = 1;
	revs->topo_order = 1;
	revs->lifo = 1;
	
	/* re-use info from other caches if possible */
	trci = &revs->rev_cache_info;
	init_rev_cache_info(trci);
	trci->save_unique = 1;
	trci->add_to_pending = 0;
	
	setup_revisions(0, 0, revs, 0);
	if (prepare_revision_walk(revs))
		die("died preparing revision walk");
	
	if (rci->legs)
		make_legs(revs);
	
	object_nr = total_sz = 0;
	while ((commit = get_revision(revs)) != 0) {
		struct rc_object_entry object;
		int t;
		
		strbuf_setlen(&merge_paths, 0);
		strbuf_setlen(&split_paths, 0);
		
		memset(&object, 0, sizeof(object));
		object.type = OBJ_COMMIT;
		object.date = htonl(commit->date);
		hashcpy(object.sha1, commit->object.sha1);
		
		handle_paths(commit, &object, &merge_paths, &split_paths);
		
		if (object.is_end) {
			strbuf_add(&endlist, object.sha1, 20);
			if (ends)
				commit_list_insert(commit, ends);
		}
		/* the two *aren't* mutually exclusive */
		if (object.is_start) {
			strbuf_add(&startlist, object.sha1, 20);
			if (starts)
				commit_list_insert(commit, starts);
		}
		
		commit->indegree = 0;
		
		add_object_entry(0, &object, &merge_paths, &split_paths);
		object_nr++;
		
		if (rci->objects) {
			if (rci->fuse_me && (t = add_objects_verbatim(rci, commit)) >= 0) {
				/* yay!  we did it! */
				object_nr += t;
			} else {
				/* add all unique children for this commit */
				add_object_entry(commit->tree->object.sha1, 0, 0, 0);
				object_nr++;
				
				if (!object.is_end)
					object_nr += add_unique_objects(commit);
			}
		}
		
		/* print every ~1MB or so */
		if (buffer.len > 1000000) {
			write_in_full(fd, buffer.buf, buffer.len);
			total_sz += buffer.len;
			
			strbuf_setlen(&buffer, 0);
		}
	}
	
	if (buffer.len) {
		write_in_full(fd, buffer.buf, buffer.len);
		total_sz += buffer.len;
	}
	
	/* go ahead a free some stuff... */
	strbuf_release(&buffer);
	strbuf_release(&merge_paths);
	strbuf_release(&split_paths);
	if (path_sz)
		free(paths);
	while (path_track_alloc)
		remove_path_track(&path_track_alloc, 1);
	
	/* the meaning of the hash name is more or less irrelevant, it's the uniqueness that matters */
	strbuf_add(&endlist, startlist.buf, startlist.len);
	git_SHA1_Init(&ctx);
	git_SHA1_Update(&ctx, endlist.buf, endlist.len);
	git_SHA1_Final(sha1, &ctx);
	
	/* now actually initialize header */
	strcpy(head.signature, "REVCACHE");
	head.version = SUPPORTED_REVCACHE_VERSION;
	
	head.object_nr = htonl(object_nr);
	head.size = htonl(ntohl(head.ofs_objects) + total_sz);
	head.path_nr = htons(path_nr);
	hashcpy(head.sha1, sha1);
	
	/* some info! */
	fprintf(stderr, "objects: %d\n", object_nr);
	fprintf(stderr, "paths: %d\n", path_nr);
	
	lseek(fd, 0, SEEK_SET);
	xwrite(fd, &head, sizeof(head));
	
	if (rci->make_index && make_cache_index(rci, sha1, fd, ntohl(head.size)) < 0)
		die("can't update index");
	
	close(fd);
	
	newfile = git_path("rev-cache/%s", sha1_to_hex(sha1));
	if (rename(file, newfile))
		die("can't move temp file");
	
	/* let our caller know what we've just made */
	if (cache_sha1)
		hashcpy(cache_sha1, sha1);
	
	strbuf_release(&endlist);
	strbuf_release(&startlist);
	
	return 0;
}


static int index_sort_hash(const void *a, const void *b)
{
	return hashcmp(IE_CAST(a)->sha1, IE_CAST(b)->sha1);
}

static int write_cache_index(struct strbuf *body)
{
	struct rc_index_header whead;
	struct lock_file *lk;
	int fd, i;
	
	/* clear index map if loaded */
	if (idx_map) {
		munmap(idx_map, idx_size);
		idx_map = 0;
	}
	
	lk = xcalloc(sizeof(struct lock_file), 1);
	fd = hold_lock_file_for_update(lk, git_path("rev-cache/index"), 0);
	if (fd < 0) {
		free(lk);
		return -1;
	}
	
	/* endianness yay! */
	memset(&whead, 0, sizeof(whead));
	memcpy(whead.signature, "REVINDEX", 8);
	whead.version = idx_head.version;
	whead.ofs_objects = htonl(idx_head.ofs_objects);
	whead.object_nr = htonl(idx_head.object_nr);
	whead.cache_nr = idx_head.cache_nr;
	whead.max_date = htonl(idx_head.max_date);
	
	write(fd, &whead, sizeof(struct rc_index_header));
	write_in_full(fd, idx_caches, idx_head.cache_nr * 20);
	
	for (i = 0; i <= 0xff; i++)
		fanout[i] = htonl(fanout[i]);
	write_in_full(fd, fanout, 0x100 * sizeof(uint32_t));
	
	write_in_full(fd, body->buf, body->len);
	
	if (commit_lock_file(lk) < 0)
		return -2;
	
	/* lk freed by lockfile.c */
	
	return 0;
}

int make_cache_index(struct rev_cache_info *rci, unsigned char *cache_sha1, 
	int fd, unsigned int size)
{
	struct strbuf buffer;
	int i, cache_index, cur;
	unsigned char *map;
	unsigned long max_date;
	
	maybe_fill_with_defaults(rci);
	
	if (!idx_map)
		init_index();
	
	lseek(fd, 0, SEEK_SET);
	map = xmmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		return -1;
	
	strbuf_init(&buffer, 0);
	if (idx_map) {
		strbuf_add(&buffer, idx_map + fanout[0], fanout[0x100] - fanout[0]);
	} else {
		/* not an update */
		memset(&idx_head, 0, sizeof(struct rc_index_header));
		idx_caches = 0;
		
		strcpy(idx_head.signature, "REVINDEX");
		idx_head.version = SUPPORTED_REVINDEX_VERSION;
		idx_head.ofs_objects = sizeof(struct rc_index_header) + 0x100 * sizeof(uint32_t);
	}
	
	/* are we remaking a slice? */
	for (i = 0; i < idx_head.cache_nr; i++)
		if (!hashcmp(idx_caches + i * 20, cache_sha1))
			break;
	
	if (i == idx_head.cache_nr) {
		cache_index = idx_head.cache_nr++;
		idx_head.ofs_objects += 20;
		
		idx_caches = xrealloc(idx_caches, idx_head.cache_nr * 20);
		hashcpy(idx_caches + cache_index * 20, cache_sha1);
	} else
		cache_index = i;
	
	i = sizeof(struct rc_slice_header); /* offset */
	max_date = idx_head.max_date;
	while (i < size) {
		struct rc_index_entry index_entry, *entry;
		struct rc_object_entry *object_entry = OE_CAST(map + i);
		unsigned long date;
		int pos = i;
		
		i += ACTUAL_OBJECT_ENTRY_SIZE(object_entry);
		
		if (object_entry->type != OBJ_COMMIT)
			continue;
		
		/* don't include ends; otherwise we'll find ourselves in loops */
		if (object_entry->is_end)
			continue;
		
		/* handle index duplication
		 * -> keep old copy unless new one is a start -- based on expected usage, older ones will be more 
		 * likely to lead to greater slice traversals than new ones
		 * should we allow more intelligent overriding? */
		date = ntohl(object_entry->date);
		if (date > idx_head.max_date) {
			entry = 0;
			if (date > max_date)
				max_date = date;
		} else
			entry = search_index(object_entry->sha1);
		
		if (entry && !object_entry->is_start && !rci->overwrite_all)
			continue;
		else if (entry) /* mmm, pointer arithmetic... tasty */  /* (entry-idx_map = offset, so cast is valid) */
			entry = IE_CAST(buffer.buf + (unsigned int)((unsigned char *)entry - idx_map) - fanout[0]);
		else
			entry = &index_entry;
		
		memset(entry, 0, sizeof(index_entry));
		hashcpy(entry->sha1, object_entry->sha1);
		entry->is_start = object_entry->is_start;
		entry->cache_index = cache_index;
		entry->pos = htonl(pos);
		
		if (entry == &index_entry) {
			strbuf_add(&buffer, entry, sizeof(index_entry));
			idx_head.object_nr++;
		}
		
	}
	
	idx_head.max_date = max_date;
	qsort(buffer.buf, buffer.len / IE_SIZE, IE_SIZE, index_sort_hash);
	
	/* generate fanout */
	cur = 0x00;
	for (i = 0; i < buffer.len; i += IE_SIZE) {
		struct rc_index_entry *entry = IE_CAST(buffer.buf + i);
		
		while (cur <= entry->sha1[0])
			fanout[cur++] = i + idx_head.ofs_objects;
	}
	
	while (cur <= 0xff)
		fanout[cur++] = idx_head.ofs_objects + buffer.len;
	
	/* BOOM! */
	if (write_cache_index(&buffer))
		return -1;
	
	munmap(map, size);
	strbuf_release(&buffer);
	
	/* idx_map is unloaded without cleanup_cache_slices(), so regardless of previous index existence 
	 * we can still free this up */
	free(idx_caches);
	
	return 0;
}


void starts_from_slices(struct rev_info *revs, unsigned int flags, unsigned char *which, int n)
{
	struct commit *commit;
	int i;
	
	if (!idx_map)
		init_index();
	if (!idx_map)
		return;
	
	for (i = idx_head.ofs_objects; i < idx_size; i += IE_SIZE) {
		struct rc_index_entry *entry = IE_CAST(idx_map + i);
		
		if (!entry->is_start)
			continue;
		
		/* only include entries in 'which' slices */
		if (n) {
			int j;
			
			for (j = 0; j < n; j++)
				if (!hashcmp(idx_caches + entry->cache_index * 20, which + j * 20))
					break;
			
			if (j == n)
				continue;
		}
		
		commit = lookup_commit(entry->sha1);
		if (!commit)
			continue;
		
		commit->object.flags |= flags;
		add_pending_object(revs, &commit->object, 0);
	}
	
}


struct slice_fd_time {
	unsigned char sha1[20];
	int fd;
	struct stat fi;
};

int slice_time_sort(const void *a, const void *b)
{
	unsigned long at, bt;
	
	at = ((struct slice_fd_time *)a)->fi.st_ctime;
	bt = ((struct slice_fd_time *)b)->fi.st_ctime;
	
	if (at == bt)
		return 0;
	
	return at > bt ? 1 : -1;
}

int regenerate_cache_index(struct rev_cache_info *rci)
{
	DIR *dirh;
	int i;
	struct slice_fd_time info;
	struct strbuf slices;
	
	/* first remove old index if it exists */
	unlink_or_warn(git_path("rev-cache/index"));
	
	strbuf_init(&slices, 0);
	
	dirh = opendir(git_path("rev-cache"));
	if (dirh) {
		struct dirent *de;
		struct stat fi;
		int fd;
		unsigned char sha1[20];
		
		while ((de = readdir(dirh))) {
			if (de->d_name[0] == '.')
				continue;
			
			if (get_sha1_hex(de->d_name, sha1))
				continue;
			
			/* open with RDWR because of mmap call in make_cache_index() */
			fd = open_cache_slice(sha1, O_RDONLY);
			if (fd < 0 || fstat(fd, &fi)) {
				warning("bad cache found [%s]; fuse recommended", de->d_name);
				if (fd > 0)
					close(fd);
				continue;
			}
			
			hashcpy(info.sha1, sha1);
			info.fd = fd;
			memcpy(&info.fi, &fi, sizeof(struct stat));
			
			strbuf_add(&slices, &info, sizeof(info));
		}
		
		closedir(dirh);
	}
	
	/* we want oldest first -> upon overlap, older slices are more likely to have a larger section, 
	 * as of the overlapped commit */
	qsort(slices.buf, slices.len / sizeof(info), sizeof(info), slice_time_sort);
	
	for (i = 0; i < slices.len; i += sizeof(info)) {
		struct slice_fd_time *infop = (struct slice_fd_time *)(slices.buf + i);
		struct stat *fip = &infop->fi;
		int fd = infop->fd;
		
		if (make_cache_index(rci, infop->sha1, fd, fip->st_size) < 0)
			die("error writing cache");
		
		close(fd);
	}
	
	strbuf_release(&slices);
	
	return 0;
}

static int add_slices_for_fuse(struct rev_cache_info *rci, struct string_list *files, struct strbuf *ignore)
{
	unsigned char sha1[20];
	char base[PATH_MAX];
	int baselen, i, slice_nr = 0;
	struct stat fi;
	DIR *dirh;
	struct dirent *de;
	
	strncpy(base, git_path("rev-cache"), sizeof(base));
	baselen = strlen(base);
	
	dirh = opendir(base);
	if (!dirh)
		return 0;
	
	while ((de = readdir(dirh))) {
		if (de->d_name[0] == '.')
			continue;
		
		base[baselen] = '/';
		strncpy(base + baselen + 1, de->d_name, sizeof(base) - baselen - 1);
		
		if (get_sha1_hex(de->d_name, sha1)) {
			/* whatever it is, we don't need it... */
			string_list_insert(base, files);
			continue;
		}
		
		/* _theoretically_ it is possible a slice < ignore_size to map objects not covered by, yet reachable from, 
		 * a slice >= ignore_size, meaning that we could potentially delete an 'unfused' slice; but if that 
		 * ever *did* happen their cache structure'd be so fucked up they might as well refuse the entire thing.
		 * and at any rate the worst it'd do is make rev-list revert to standard walking in that (small) bit.
		 */
		if (rci->ignore_size) {
			if (stat(base, &fi))
				warning("can't query file %s\n", base);
			else if (fi.st_size >= rci->ignore_size) {
				strbuf_add(ignore, sha1, 20);
				continue;
			}
		} else {
			/* check if a pointer */
			struct cache_slice_pointer ptr;
			int fd = open(base, O_RDONLY);
			
			if (fd < 0)
				goto dont_save;
			if (sizeof(ptr) != read_in_full(fd, &ptr, sizeof(ptr)))
				goto dont_save;
			
			close(fd);
			if (!strcmp(ptr.signature, "REVCOPTR")) {
				strbuf_add(ignore, sha1, 20);
				continue;
			}
		}
		
dont_save:
		for (i = idx_head.cache_nr - 1; i >= 0; i--) {
			if (!hashcmp(idx_caches + i * 20, sha1))
				break;
		}
		
		if (i >= 0)
			rci->maps[i].size = 1;
		
		string_list_insert(base, files);
		slice_nr++;
	}
	
	closedir(dirh);
	
	return slice_nr;
}

/* the most work-intensive attributes in the cache are the unique objects and size, both 
 * of which can be re-used.  although path structures will be isomorphic, path generation is 
 * not particularly expensive, and at any rate we need to re-sort the commits */
int fuse_cache_slices(struct rev_cache_info *rci, struct rev_info *revs)
{
	unsigned char cache_sha1[20];
	struct string_list files = {0, 0, 0, 1}; /* dup */
	struct strbuf ignore;
	int i;
	
	maybe_fill_with_defaults(rci);
	
	if (!idx_map)
		init_index();
	if (!idx_map)
		return -1;
	
	strbuf_init(&ignore, 0);
	rci->maps = xcalloc(idx_head.cache_nr, sizeof(struct rev_cache_slice_map));
	if (add_slices_for_fuse(rci, &files, &ignore) <= 1) {
		printf("nothing to fuse\n");
		return 1;
	}
	
	if (ignore.len) {
		starts_from_slices(revs, UNINTERESTING, (unsigned char *)ignore.buf, ignore.len / 20);
		strbuf_release(&ignore);
	}
	
	/* initialize mappings */
	for (i = idx_head.cache_nr - 1; i >= 0; i--) {
		struct rev_cache_slice_map *map = rci->maps + i;
		struct stat fi;
		int fd;
		
		if (!map->size)
			continue;
		map->size = 0;
		
		/* pointers are never fused, so we can use open directly */
		fd = open(git_path("rev-cache/%s", sha1_to_hex(idx_caches + i * 20)), O_RDONLY);
		if (fd <= 0 || fstat(fd, &fi))
			continue;
		if (fi.st_size < sizeof(struct cache_slice_header))
			continue;
		
		map->map = xmmap(0, fi.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (map->map == MAP_FAILED)
			continue;
		
		close(fd);
		map->size = fi.st_size;
	}
	
	rci->make_index = 0;
	rci->fuse_me = 1;
	if (make_cache_slice(rci, revs, 0, 0, cache_sha1) < 0)
		die("can't make cache slice");
	
	printf("%s\n", sha1_to_hex(cache_sha1));
	
	/* clean up time! */
	for (i = idx_head.cache_nr - 1; i >= 0; i--) {
		struct rev_cache_slice_map *map = rci->maps + i;
		
		if (!map->size)
			continue;
		
		munmap(map->map, map->size);
	}
	free(rci->maps);
	cleanup_cache_slices();
	
	for (i = 0; i < files.nr; i++) {
		char *name = files.items[i].string;
		
		fprintf(stderr, "removing %s\n", name);
		unlink_or_warn(name);
	}
	
	string_list_clear(&files, 0);
	
	return regenerate_cache_index(rci);
}

static int verify_cache_slice(const char *slice_path, unsigned char *sha1)
{
	struct cache_slice_header head;
	int fd, len, retval = -1;
	unsigned char *map = MAP_FAILED;
	struct stat fi;
	
	len = strlen(slice_path);
	if (len < 40)
		return -2;
	if (get_sha1_hex(slice_path + len - 40, sha1))
		return -3;
	
	fd = open(slice_path, O_RDONLY);
	if (fd == -1)
		goto end;
	if (fstat(fd, &fi) || fi.st_size < sizeof(head))
		goto end;
	
	map = xmmap(0, sizeof(head), PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		goto end;
	if (get_cache_slice_header(sha1, map, fi.st_size, &head))
		goto end;
	
	retval = 0;
	
end:
	if (map != MAP_FAILED)
		munmap(map, sizeof(head));
	if (fd > 0)
		close(fd);
	
	return retval;
}

int make_cache_slice_pointer(struct rev_cache_info *rci, const char *slice_path)
{
	struct cache_slice_pointer ptr;
	int fd;
	unsigned char sha1[20];
	
	maybe_fill_with_defaults(rci);
	rci->overwrite_all = 1;
	
	if (verify_cache_slice(slice_path, sha1) < 0)
		return -1;
	
	strcpy(ptr.signature, "REVCOPTR");
	ptr.version = SUPPORTED_REVCOPTR_VERSION;
	strcpy(ptr.path, make_nonrelative_path(slice_path));
	
	fd = open(git_path("rev-cache/%s", sha1_to_hex(sha1)), O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return -2;
	
	write_in_full(fd, &ptr, sizeof(ptr));
	make_cache_index(rci, sha1, fd, sizeof(ptr));
	
	close(fd);
	
	return 0;
}
