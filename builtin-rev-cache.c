#include "cache.h"
#include "object.h"
#include "commit.h"
#include "diff.h"
#include "revision.h"

#define DEFAULT_IGNORE_SLICE_SIZE		5000000 /* in bytes */

/* porcelain for rev-cache.c */
static int handle_add(int argc, const char *argv[]) /* args beyond this command */
{
	struct rev_info revs;
	struct rev_cache_info rci;
	char dostdin = 0;
	unsigned int flags = 0;
	int i, retval;
	unsigned char cache_sha1[20];
	
	init_revisions(&revs, 0);
	init_rci(&rci);
	
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--stdin"))
			dostdin = 1;
		else if (!strcmp(argv[i], "--fresh"))
			starts_from_slices(&revs, UNINTERESTING, 0, 0);
		else if (!strcmp(argv[i], "--not"))
			flags ^= UNINTERESTING;
		else if (!strcmp(argv[i], "--legs"))
			rci.legs = 1;
		else if (!strcmp(argv[i], "--noobjects"))
			rci.objects = 0;
		else if (!strcmp(argv[i], "--all")) {
			const char *args[2];
			int argn = 0;
			
			args[argn++] = "rev-list";
			args[argn++] = "--all";
			setup_revisions(argn, args, &revs, 0);
		} else
			handle_revision_arg(argv[i], &revs, flags, 1);
	}
	
	if (dostdin) {
		char line[1000];
		
		flags = 0;
		while (fgets(line, sizeof(line), stdin)) {
			int len = strlen(line);
			while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
				line[--len] = 0;
			
			if (!len)
				break;
			
			if (!strcmp(line, "--not"))
				flags ^= UNINTERESTING;
			else
				handle_revision_arg(line, &revs, flags, 1);
		}
	}
	
	retval = make_cache_slice(&rci, &revs, 0, 0, cache_sha1);
	if (retval < 0)
		return retval;
	
	printf("%s\n", sha1_to_hex(cache_sha1));
	
	return 0;
}

static int handle_walk(int argc, const char *argv[])
{
	struct commit *commit;
	struct rev_info revs;
	struct commit_list *queue, *work, **qp;
	unsigned char *sha1p, *sha1pt;
	unsigned long date = 0;
	unsigned int flags = 0;
	int retval, slop = 5, i;
	
	init_revisions(&revs, 0);
	
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--not"))
			flags ^= UNINTERESTING;
		else if (!strcmp(argv[i], "--objects"))
			revs.tree_objects = revs.blob_objects = 1;
		else
			handle_revision_arg(argv[i], &revs, flags, 1);
	}
	
	work = 0;
	sha1p = 0;
	for (i = 0; i < revs.pending.nr; i++) {
		commit = lookup_commit(revs.pending.objects[i].item->sha1);
		
		sha1pt = get_cache_slice(commit);
		if (!sha1pt)
			die("%s: not in a cache slice", sha1_to_hex(commit->object.sha1));
		
		if (!i)
			sha1p = sha1pt;
		else if (sha1p != sha1pt)
			die("walking porcelain is /per/ cache slice; commits cannot be spread out amoung several");
		
		insert_by_date(commit, &work);
	}
	
	if (!sha1p)
		die("nothing to traverse!");
	
	queue = 0;
	qp = &queue;
	commit = pop_commit(&work);
	retval = traverse_cache_slice(&revs, sha1p, commit, &date, &slop, &qp, &work);
	if (retval < 0)
		return retval;
	
	printf("queue:\n");
	while ((commit = pop_commit(&queue)) != 0) {
		printf("%s\n", sha1_to_hex(commit->object.sha1));
	}
	
	printf("work:\n");
	while ((commit = pop_commit(&work)) != 0) {
		printf("%s\n", sha1_to_hex(commit->object.sha1));
	}
	
	printf("pending:\n");
	for (i = 0; i < revs.pending.nr; i++) {
		struct object *obj = revs.pending.objects[i].item;
		
		/* unfortunately, despite our careful generation, object duplication *is* a possibility...
		 * (eg. same object introduced into two different branches) */
		if (obj->flags & SEEN)
			continue;
		
		printf("%s\n", sha1_to_hex(revs.pending.objects[i].item->sha1));
		obj->flags |= SEEN;
	}
	
	return 0;
}

static int handle_fuse(int argc, const char *argv[])
{
	struct rev_info revs;
	struct rev_cache_info rci;
	const char *args[5];
	int i, argn = 0;
	char add_all = 0;
	
	init_revisions(&revs, 0);
	init_rci(&rci);
	args[argn++] = "rev-list";
	
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--all")) {
			args[argn++] = "--all";
			setup_revisions(argn, args, &revs, 0);
			add_all = 1;
		} else if (!strcmp(argv[i], "--noobjects")) 
			rci.objects = 0;
		else if (!strncmp(argv[i], "--ignore-size", 13)) {
			int sz = DEFAULT_IGNORE_SLICE_SIZE;
			
			if (argv[i][13] == '=')
				sz = atoi(argv[i] + 14);
			
			rci.ignore_size = sz;
		} else 
			continue;
	}
	
	if (!add_all)
		starts_from_slices(&revs, 0, 0, 0);
	
	return coagulate_cache_slices(&rci, &revs);
}

static int handle_index(int argc, const char *argv[])
{
	return regenerate_cache_index();
}

static int handle_help(void)
{
	char *usage = "\
usage:\n\
git-rev-cache COMMAND [options] [<commit-id>...]\n\
commands:\n\
  add    - add revisions to the cache.  reads commit ids from stdin, \n\
           formatted as: START START ... --not END END ...\n\
           options:\n\
            --all             use all branch heads as starts\n\
            --fresh           exclude everything already in a cache slice\n\
            --stdin           also read commit ids from stdin (same form as cmd)\n\
            --legs            ensure branch is entirely self-contained\n\
            --noobjects       don't add non-commit objects to slice\n\
  walk   - walk a cache slice based on set of commits; formatted as add\n\
           options:\n\
           --objects          include non-commit objects in traversals\n\
  fuse   - coagulate cache slices into a single cache.\n\
           options:\n\
            --all             include all objects in repository\n\
            --noobjects       don't add non-commit objects to slice\n\
            --ignore-size[=N] ignore slices of size >= N; defaults to ~5MB\n\
  index  - regnerate the cache index.";
	
	puts(usage);
	
	return 0;
}

int cmd_rev_cache(int argc, const char *argv[], const char *prefix)
{
	const char *arg;
	int r;
	
	git_config(git_default_config, NULL);
	
	if (argc > 1)
		arg = argv[1];
	else
		arg = "";
	
	argc -= 2;
	argv += 2;
	if (!strcmp(arg, "add"))
		r = handle_add(argc, argv);
	else if (!strcmp(arg, "fuse"))
		r = handle_fuse(argc, argv);
	else if (!strcmp(arg, "walk"))
		r = handle_walk(argc, argv);
	else if (!strcmp(arg, "index"))
		r = handle_index(argc, argv);
	else
		return handle_help();
	
	fprintf(stderr, "final return value: %d\n", r);
	
	return 0;
}
