#include "builtin.h"
#include "cache.h"
#include "pack.h"
#include "pack-revindex.h"
#include "parse-options.h"

#define MAX_CHAIN 50

static void show_pack_info(struct packed_git *p)
{
	uint32_t nr_objects, i;
	int cnt;
	unsigned long chain_histogram[MAX_CHAIN+1], baseobjects;

	nr_objects = p->num_objects;
	memset(chain_histogram, 0, sizeof(chain_histogram));
	baseobjects = 0;

	for (i = 0; i < nr_objects; i++) {
		const unsigned char *sha1;
		unsigned char base_sha1[20];
		const char *type;
		unsigned long size;
		unsigned long store_size;
		off_t offset;
		unsigned int delta_chain_length;

		sha1 = nth_packed_object_sha1(p, i);
		if (!sha1)
			die("internal error pack-check nth-packed-object");
		offset = nth_packed_object_offset(p, i);
		type = packed_object_info_detail(p, offset, &size, &store_size,
						 &delta_chain_length,
						 base_sha1);
		printf("%s ", sha1_to_hex(sha1));
		if (!delta_chain_length) {
			printf("%-6s %lu %lu %"PRIuMAX"\n",
			       type, size, store_size, (uintmax_t)offset);
			baseobjects++;
		}
		else {
			printf("%-6s %lu %lu %"PRIuMAX" %u %s\n",
			       type, size, store_size, (uintmax_t)offset,
			       delta_chain_length, sha1_to_hex(base_sha1));
			if (delta_chain_length <= MAX_CHAIN)
				chain_histogram[delta_chain_length]++;
			else
				chain_histogram[0]++;
		}
	}

	if (baseobjects)
		printf("non delta: %lu object%s\n",
		       baseobjects, baseobjects > 1 ? "s" : "");

	for (cnt = 1; cnt <= MAX_CHAIN; cnt++) {
		if (!chain_histogram[cnt])
			continue;
		printf("chain length = %d: %lu object%s\n", cnt,
		       chain_histogram[cnt],
		       chain_histogram[cnt] > 1 ? "s" : "");
	}
	if (chain_histogram[0])
		printf("chain length > %d: %lu object%s\n", MAX_CHAIN,
		       chain_histogram[0],
		       chain_histogram[0] > 1 ? "s" : "");
}

static int verify_one_pack(const char *path, int verbose)
{
	char arg[PATH_MAX];
	int len;
	struct packed_git *pack;
	int err;

	len = strlcpy(arg, path, PATH_MAX);
	if (len >= PATH_MAX)
		return error("name too long: %s", path);

	/*
	 * In addition to "foo.idx" we accept "foo.pack" and "foo";
	 * normalize these forms to "foo.idx" for add_packed_git().
	 */
	if (has_extension(arg, ".pack")) {
		strcpy(arg + len - 5, ".idx");
		len--;
	} else if (!has_extension(arg, ".idx")) {
		if (len + 4 >= PATH_MAX)
			return error("name too long: %s.idx", arg);
		strcpy(arg + len, ".idx");
		len += 4;
	}

	/*
	 * add_packed_git() uses our buffer (containing "foo.idx") to
	 * build the pack filename ("foo.pack").  Make sure it fits.
	 */
	if (len + 1 >= PATH_MAX) {
		arg[len - 4] = '\0';
		return error("name too long: %s.pack", arg);
	}

	pack = add_packed_git(arg, len, 1);
	if (!pack)
		return error("packfile %s not found.", arg);

	install_packed_git(pack);
	err = verify_pack(pack);

	if (verbose) {
		if (err)
			printf("%s: bad\n", pack->pack_name);
		else {
			show_pack_info(pack);
			printf("%s: ok\n", pack->pack_name);
		}
	}

	return err;
}

static const char * const verify_pack_usage[] = {
	"git verify-pack [-v|--verbose] <pack>...",
	NULL
};

int cmd_verify_pack(int argc, const char **argv, const char *prefix)
{
	int err = 0;
	int verbose = 0;
	int i;
	const struct option verify_pack_options[] = {
		OPT__VERBOSE(&verbose),
		OPT_END()
	};

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix, verify_pack_options,
			     verify_pack_usage, 0);
	if (argc < 1)
		usage_with_options(verify_pack_usage, verify_pack_options);
	for (i = 0; i < argc; i++) {
		if (verify_one_pack(argv[i], verbose))
			err = 1;
		discard_revindex();
	}

	return err;
}
